#include <grpc++/grpc++.h>
#include <sentencepiece_processor.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <random>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "llm.grpc.pb.h"
#include "ppl/common/log.h"
#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

using namespace grpc;
using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using namespace std::chrono;
using namespace ppl::llm;

static pthread_cond_t finished_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static std::unordered_map<int, std::string> rsp_stream_store;
static int finished_cnt = 0;
static int num_request = 0;
struct TidRecord {
    int prompt_len;
    int output_len;
    bool is_prefill = true;
    std::chrono::_V2::system_clock::time_point prefill_time;
    std::chrono::_V2::system_clock::time_point finished_time;
    std::chrono::_V2::system_clock::time_point benchmark_start_time;
};
static std::unordered_map<int64_t, TidRecord> tid_record_map;

void SampleRequest(const std::string& dataset_path, const sentencepiece::SentencePieceProcessor& tokenizer,
                   std::vector<std::shared_ptr<proto::BatchedRequest>>* req_list) {
    std::ifstream ifs(dataset_path);
    rapidjson::IStreamWrapper isw(ifs);
    rapidjson::Document root;
    root.ParseStream(isw);
    if (root.HasParseError()) {
        return;
    }
    LOG(INFO) << "request size: " << root.Size();
    uint64_t tid = 0;
    for (size_t i = 0; i < root.Size(); ++i) {
        const auto& convs = root[i]["conversations"];
        const std::string prompt = convs[0]["value"].GetString();
        const std::string ans = convs[1]["value"].GetString();

        std::vector<int> prompt_token_ids;
        std::vector<int> ans_token_ids;
        tokenizer.Encode(prompt, &prompt_token_ids);
        tokenizer.Encode(ans, &ans_token_ids);

        auto batch_req = std::make_shared<proto::BatchedRequest>(); // batch_size = 1
        auto* req = batch_req->add_req();
        req->set_id(tid);
        req->set_prompt(prompt);
        req->set_temperature(1);
        req->set_generation_length(ans_token_ids.size());
        req_list->push_back(batch_req);

        auto& tid_record = tid_record_map.emplace(tid, TidRecord()).first->second;
        tid_record.prompt_len = prompt_token_ids.size();
        tid_record.output_len = ans_token_ids.size();
        tid++;
    }
}

enum CallStatus { CREATE, PROCESS, PROCESSED, FINISH, FAILED };

class GenerationClientAsync {
public:
    GenerationClientAsync(std::shared_ptr<Channel> channel) : stub_(proto::LLMService::NewStub(channel)) {}

    // void Generation(const std::vector<std::shared_ptr<proto::BatchedRequest>> req_list, float request_rate) {
    //     if(request_rate == 0.0){

    //     }
    //     for (size_t i = 0; i < req_list.size(); i++) {
    //         const auto& req_batch = *req_list[i];
    //         AsyncClientCall* call = new AsyncClientCall;

    //         call->response_reader = stub_->PrepareAsyncGeneration(&call->context, req_batch, &cq_);
    //         call->response_reader->StartCall((void*)call);
    //     }
    //     pthread_mutex_lock(&lock);
    //     while (finished_cnt < num_request) {
    //         pthread_cond_wait(&finished_cond, &lock);
    //     }
    //     pthread_mutex_unlock(&lock);
    // }


    // Function to generate requests
    void Generation(const std::vector<std::shared_ptr<proto::BatchedRequest>> req_list, float request_rate) {
        if (request_rate <= 0.1) {
            // Handle the case where request_rate is zero (no delay)
            for (size_t i = 0; i < req_list.size(); i++) {
                const auto& req_batch = *req_list[i];
                AsyncClientCall* call = new AsyncClientCall;

                call->response_reader = stub_->PrepareAsyncGeneration(&call->context, req_batch, &cq_);
                call->response_reader->StartCall((void*)call);
            }
            pthread_mutex_lock(&lock);
            while (finished_cnt < num_request) {
                pthread_cond_wait(&finished_cond, &lock);
            }
            pthread_mutex_unlock(&lock);
            return;
        }

        // Create a random number generator for exponential distribution
        std::default_random_engine generator;
        std::exponential_distribution<double> distribution(1.0 / request_rate);

        for (size_t i = 0; i < req_list.size(); i++) {
            const auto& req_batch = *req_list[i];
            AsyncClientCall* call = new AsyncClientCall;

            tid_record_map[i].benchmark_start_time = std::chrono::high_resolution_clock::now();

            call->response_reader = stub_->PrepareAsyncGeneration(&call->context, req_batch, &cq_);
            call->response_reader->StartCall((void*)call);

            // Generate a random delay based on exponential distribution
            // double interval = distribution(generator);
            double interval = 1.0 / request_rate;
            LOG(INFO) << "the " << i << "th req interval: " << interval << "s";

            // Sleep for the calculated interval (milliseconds)
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(interval * 1000)));
        }

        pthread_mutex_lock(&lock);
        while (finished_cnt < num_request) {
            pthread_cond_wait(&finished_cond, &lock);
        }
        pthread_mutex_unlock(&lock);
    }

    // Loop while listening for completed responses.
    void AsyncCompleteRpc() {
        void* got_tag;
        bool ok = false;
        // Block until the next result is available in the completion queue "cq".
        LOG(INFO) << "Wait for response";
        while (cq_.Next(&got_tag, &ok)) {
            if (!got_tag) {
                LOG(ERROR) << "Get tag failed";
            }

            // The tag in this example is the memory location of the call object
            AsyncClientCall* call = static_cast<AsyncClientCall*>(got_tag);
            call->HandleResponse(ok);

            if (finished_cnt >= num_request) {
                pthread_cond_signal(&finished_cond);
                break;
            }
        }
    }

private:
    struct AsyncClientCall {
        void HandleResponse(bool responseStatus) {
            switch (callStatus_) {
                case CREATE:
                    if (responseStatus) {
                        response_reader->Read(&reply, (void*)this);
                        callStatus_ = PROCESS;
                    } else {
                        response_reader->Finish(&status, (void*)this);
                        callStatus_ = FINISH;
                    }
                    break;
                case PROCESS:
                    if (responseStatus) {
                        auto& rsp = this->reply;
                        int tid = rsp.id();
                        if (tid_record_map[tid].is_prefill == true) {
                            tid_record_map[tid].prefill_time = std::chrono::high_resolution_clock::now();
                            tid_record_map[tid].is_prefill = false;
                        }
                        const std::string& rsp_stream = rsp.generated();
                        rsp_stream_store[tid] += rsp_stream;
                        response_reader->Read(&reply, (void*)this);
                    } else {
                        response_reader->Finish(&status, (void*)this);
                        callStatus_ = FINISH;
                    }
                    break;
                case FINISH:
                    __sync_fetch_and_add(&finished_cnt, 1);
                    tid_record_map[reply.id()].finished_time = std::chrono::high_resolution_clock::now();
                    LOG(INFO) << "Finish: " << finished_cnt << "/" << num_request;
                    if (status.ok()) {
                        LOG(INFO) << "Server Response Completed: " << reply.id();
                    } else {
                        LOG(ERROR) << "RPC failed";
                    }
                    delete this;
                    break;
                default:
                    LOG(ERROR) << "impossible or invalid status";
                    break;
            }
        };

        CallStatus callStatus_ = CREATE;
        proto::Response reply;
        ClientContext context;
        Status status;
        std::unique_ptr<ClientAsyncReader<proto::Response>> response_reader;
    };

    std::unique_ptr<proto::LLMService::Stub> stub_;

    CompletionQueue cq_;
};

int main(int argc, char const* argv[]) {
    if (argc < 5) {
    // if (argc < 4) {
        std::cerr << "usage: " << argv[0] << " host:port tokenizer_path samples_json_path request_rate" << std::endl;
        // std::cerr << "usage: " << argv[0] << " host:port tokenizer_path samples_json_path" << std::endl;
        return -1;
    }
    const std::string target_str = argv[1];
    const std::string tokenizer_path = argv[2]; // LLaMA/tokenizer.model
    const std::string data_path = argv[3]; // ./samples_1024.json
    const std::string rqs_str = argv[4]; // request_rate, such as 20

    float request_rate;
    try {
        // Convert the string to a float
        request_rate = std::stof(rqs_str);

        // Now 'request_rate' contains the floating-point value
        std::cout << "Request Rate (float): " << request_rate << std::endl;
    } catch (const std::invalid_argument& e) {
        // Handle the case where the string cannot be converted to a float
        std::cerr << "Invalid argument: " << rqs_str << std::endl;
        return -1;
    } catch (const std::out_of_range& e) {
        // Handle the case where the string is out of the range of a float
        std::cerr << "Out of range: " << rqs_str << std::endl;
        return -1;
    }

    sentencepiece::SentencePieceProcessor tokenizer;
    const auto tokenizer_status = tokenizer.Load(tokenizer_path);
    if (!tokenizer_status.ok()) {
        LOG(ERROR) << tokenizer_status.ToString();
        return -1;
    }
    LOG(INFO) << "VOCAB_SIZE: " << tokenizer.GetPieceSize() << "; BOS ID: " << tokenizer.bos_id()
              << "; EOS ID: " << tokenizer.eos_id() << "; PAD ID: " << tokenizer.pad_id();

    // Define and initialize your data structures
    std::map<std::string, rapidjson::Value> throughput_dict;
    // std::vector<float> request_rates = {1, 5, 10, 20, 50, 100, 200, 0}; // Example request rates
    // std::vector<float> request_rates = {200, 100, 50, 10, 0, 5, 1}; // Example request rates
    // std::vector<float> request_rates = {200, 100}; // Example request rates
    std::vector<float> request_rates = {request_rate}; // Example request rates

    // Create a RapidJSON document for the final output
    rapidjson::Document json_throughput_dict;
    json_throughput_dict.SetObject();

    // Create a RapidJSON array for the request rates
    rapidjson::Value request_rate_array(rapidjson::kArrayType);

    GenerationClientAsync generator(grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials()));

    // Loop through request rates
    for (auto request_rate : request_rates) {

        std::vector<std::shared_ptr<proto::BatchedRequest>> req_list;
        SampleRequest(data_path, tokenizer, &req_list);
        // req_list.resize(10);
        num_request = req_list.size();

        LOG(INFO) << "request rate: " << request_rate << " request/s";

        std::thread recv_thread = std::thread(&GenerationClientAsync::AsyncCompleteRpc, &generator);

        auto benchmark_start = std::chrono::high_resolution_clock::now();
        generator.Generation(req_list, request_rate);
        auto benchmark_end = std::chrono::high_resolution_clock::now();

        auto benchmark_time =
            double(std::chrono::duration_cast<std::chrono::microseconds>(benchmark_end - benchmark_start).count()) /
            1000.0 / 1000.0;

        double total_prefill_latency = 0; // ms
        double total_decode_latency_per_token = 0; // ms
        double total_prompt_latency = 0; // ms
        int total_input_tokens = 0;
        int total_gen_tokens = 0;
        for (auto it = tid_record_map.begin(); it != tid_record_map.end(); ++it) {
            auto& tid_record = it->second;

            auto benchmark_start = tid_record.benchmark_start_time;

            double prefill_latency =
                double(std::chrono::duration_cast<std::chrono::microseconds>(tid_record.prefill_time - benchmark_start)
                           .count() /
                       1000.0); // ms

            double decoding_tatency = double(std::chrono::duration_cast<std::chrono::microseconds>(
                                                 tid_record.finished_time - tid_record.prefill_time)
                                                 .count() /
                                             1000.0); // ms
            double prompt_latency =
                double(std::chrono::duration_cast<std::chrono::microseconds>(tid_record.finished_time - benchmark_start)
                           .count() /
                       1000.0); // ms
            // total_latency_per_token += (prompt_latency / tid_record.output_len);
            total_prompt_latency += prompt_latency;

            total_prefill_latency += prefill_latency;
            total_decode_latency_per_token += decoding_tatency / (tid_record.output_len - 1);

            total_input_tokens += tid_record.prompt_len;
            total_gen_tokens += tid_record.output_len;
        }
        double avg_latency_prefill = total_prefill_latency / num_request;
        double avg_latency_decode_per_token = total_decode_latency_per_token / num_request;
        double avg_latency_per_prompt = total_prompt_latency / num_request;

        fprintf(stderr, "[RESULT] benchmark time: %.2f s\n", benchmark_time);

        // 统计: avg inptu len, avg gen len, task num, total gen tokens
        fprintf(stderr, "[RESULT] request count: %d\n", num_request);
        fprintf(stderr, "[RESULT] avg input len: %d, total input len: %d\n", total_input_tokens / num_request,
                total_input_tokens);
        fprintf(stderr, "[RESULT] avg gen len: %d, total gen len: %d\n", total_gen_tokens / num_request,
                total_gen_tokens);
        fprintf(stderr, "[RESULT] time per token: %.2f ms\n", benchmark_time * 1000 / total_gen_tokens);
        fprintf(stderr, "[RESULT] avg latency prefill: %.2f ms\n", avg_latency_prefill);
        fprintf(stderr, "[RESULT] avg latency decoding: %.2f ms\n", avg_latency_decode_per_token);
        fprintf(stderr, "[RESULT] avg latency per prompt: %.2f ms\n", avg_latency_per_prompt);

        // tps1, tps2
        fprintf(stderr, "[RESULT] tokens out per sec: %.2f\n", total_gen_tokens / benchmark_time);
        fprintf(stderr, "[RESULT] tokens inout per sec: %.2f\n",
                (total_input_tokens + total_gen_tokens) / benchmark_time);
        // qps
        fprintf(stderr, "[RESULT] requests per sec: %.2f\n", num_request / benchmark_time);

        recv_thread.join();
        tid_record_map.clear();

        double total_time = benchmark_time;
        double throughput = num_request / benchmark_time;
        double avg_latency = total_prompt_latency / num_request;
        double avg_per_token_latency = total_prompt_latency / (total_input_tokens + total_gen_tokens);
        double avg_per_output_token_latency = total_prompt_latency / total_gen_tokens;

        // Create a RapidJSON object for the metrics
        rapidjson::Value metrics_object(rapidjson::kObjectType);
        metrics_object.AddMember("total_time", rapidjson::Value(total_time), json_throughput_dict.GetAllocator());
        metrics_object.AddMember("throughput", rapidjson::Value(throughput), json_throughput_dict.GetAllocator());
        metrics_object.AddMember("avg_latency", rapidjson::Value(avg_latency), json_throughput_dict.GetAllocator());
        metrics_object.AddMember("avg_per_token_latency", rapidjson::Value(avg_per_token_latency),
                                 json_throughput_dict.GetAllocator());
        metrics_object.AddMember("avg_per_output_token_latency", rapidjson::Value(avg_per_output_token_latency),
                                 json_throughput_dict.GetAllocator());

        // Create a RapidJSON array for the pair
        rapidjson::Value pair_array(rapidjson::kArrayType);

        // Add the request rate as a double
        pair_array.PushBack(rapidjson::Value(request_rate), json_throughput_dict.GetAllocator());

        // Add the metrics object to the pair array
        pair_array.PushBack(metrics_object, json_throughput_dict.GetAllocator());

        // Add the pair array to the request rate array
        request_rate_array.PushBack(pair_array, json_throughput_dict.GetAllocator());
    }

    // Store the request rate array in the 'throughput_dict' map
    throughput_dict["request rate"] = request_rate_array;

    // Save results to JSON file using RapidJSON
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    throughput_dict["request rate"].Accept(writer);

    // Save results to JSON file using RapidJSON
    std::ofstream json_file(std::string("/home/ubuntu/projects/ppl.llm.serving/throughput") + rqs_str + ".json");
    json_file << buffer.GetString();
    json_file.close();

    return 0;
}
