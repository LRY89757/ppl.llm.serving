#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "llm.grpc.pb.h"

#include <grpc++/grpc++.h>
#include <chrono>

#include <vector>
#include <tuple>
#include <map>
#include <fstream>

using namespace grpc;
using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using namespace std::chrono;
using namespace ppl::llm;

ABSL_FLAG(std::string, target, "localhost:50052", "Server address");

class GenerationClient {
public:
    GenerationClient(std::shared_ptr<Channel> channel) : stub_(proto::LLMService::NewStub(channel)) {}

    // int Generation(const std::vector<std::string>& prompts) {
    int Generation(const int bsz, const int in_len, const int out_len) {
        // Data we are sending to the server.
        ClientContext context;
        proto::BatchedRequest req_list;
        std::unordered_map<int, std::string> rsp_stream_store;

        // for (size_t i = 0; i < prompts.size(); i++) {
        //     // request
        //     auto req = req_list.add_req();
        //     req->set_id(i);
        //     req->set_prompt(prompts[i]);
        //     req->set_temperature(1);
        //     req->set_generation_length(64);
        //     rsp_stream_store[i] = "";
        // }

        for (size_t i = 0; i < bsz; i++) {
            // request
            auto req = req_list.add_req();
            req->set_id(i);
            req->set_prompt(std::to_string(in_len));
            req->set_temperature(1);
            req->set_generation_length(out_len);
            rsp_stream_store[i] = "";
        }

        // response
        proto::Response rsp;
        std::unique_ptr<ClientReader<proto::Response> > reader(stub_->Generation(&context, req_list));

        // stream chat
        auto start = system_clock::now();
        auto first_fill_time = system_clock::now();
        bool is_first_fill = true;

        while (reader->Read(&rsp)) {
            if (is_first_fill) {
                first_fill_time = system_clock::now();
                is_first_fill = false;
            }

            int tid = rsp.id();
            std::string rsp_stream = rsp.generated();
            rsp_stream_store[tid] += rsp_stream;
        }
        auto end = system_clock::now();

        std::cout << "------------------------------" << std::endl;
        std::cout << "--------- Answer -------------" << std::endl;
        std::cout << "------------------------------" << std::endl;

        for (auto rsp : rsp_stream_store) {
            std::cout << rsp.second << std::endl;
            std::cout << "--------------------" << std::endl;
        }

        auto first_till_duration = duration_cast<std::chrono::milliseconds>(first_fill_time - start);
        auto duration = duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "first fill: " << first_till_duration.count() << " ms" << std::endl;

        std::cout << "total: " << duration.count() << " ms" << std::endl;

        Status status = reader->Finish();
        if (status.ok()) {
            std::cout << "Generation rpc succeeded." << std::endl;
        } else {
            std::cerr << "Generation rpc failed." << std::endl;
            return -1;
        }
        // return 0;
        return duration.count();
    }

private:
    std::unique_ptr<proto::LLMService::Stub> stub_;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " host:port" << std::endl;
        return -1;
    }

    const std::string target_str = argv[1];

    GenerationClient generator(grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials()));

    // std::string prompt = "Building a website can be done in 10 simple steps:\n";
    // const std::vector<std::string> prompts = {3, prompt};

    // std::cout << "------------------------------" << std::endl;
    // std::cout << "--------- Question -------------" << std::endl;
    // std::cout << "------------------------------" << std::endl;

    // for (auto& str : prompts) {
    //     std::cout << str << std::endl;
    // }

    // generator.Generation(prompts);

    int std_bsz = 8;
    int std_inputlen = 32;
    int std_output_len = 128;

    std::map<std::string, std::vector<int>> all_x_axis = {
        // {"batch size", {2, 4, 8, 16, 32, 64, 128, 256, 512}},
        {"input lens", {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192}},
        // {"output lens", {128, 256, 512, 1024, 2048, 4096, 8192}}
    };

    std::vector<std::tuple<int, int, int>> batch_sizes;
    std::vector<std::tuple<int, int, int>> input_lens;
    std::vector<std::tuple<int, int, int>> output_lens;

    // for (int item : all_x_axis["batch size"]) {
    //     batch_sizes.push_back(std::make_tuple(item, std_inputlen, std_output_len));
    // }

    for (int item : all_x_axis["input lens"]) {
        input_lens.push_back(std::make_tuple(std_bsz, item, std_output_len));
    }

    // for (int item : all_x_axis["output lens"]) {
        // output_lens.push_back(std::make_tuple(std_bsz, std_inputlen, item));
    // }

    std::map<std::string, std::vector<std::tuple<int, int, int, double>>> latency_dict;

    std::map<std::string, std::vector<std::tuple<int, int, int>>> metrics = {
        // {"batch size", batch_sizes},
        {"input lens", input_lens},
        // {"output lens", output_lens}
    };

    for (const auto& kv : metrics) {
        const std::string& k = kv.first;
        const std::vector<std::tuple<int, int, int>>& v = kv.second;

        std::vector<double> latencies;

        for (size_t idx = 0; idx < v.size(); ++idx) {
            int bsz, in_len, out_len;
            std::tie(bsz, in_len, out_len) = v[idx];

            // Call the main function and store the latency
            double latency = generator.Generation(bsz, in_len, out_len);

            latencies.push_back(latency);

            // Store latency data in latency_dict
            latency_dict[k].push_back(std::make_tuple(bsz, in_len, out_len, latencies.back()));

            // Write data to a JSON file
            std::ofstream json_file("/home/ubuntu/projects/fork/latency.json");
            json_file << "{\n";
            for (const auto& entry : latency_dict) {
                json_file << "    \"" << entry.first << "\": [\n";
                for (const auto& tuple : entry.second) {
                    int b, i, o;
                    double latency;
                    std::tie(b, i, o, latency) = tuple;
                    json_file << "        [" << b << ", " << i << ", " << o << ", " << latency << "],\n";
                }
                json_file << "    ],\n";
            }
            json_file << "}\n";
            json_file.close();
        }
    }


    return 0;
}

