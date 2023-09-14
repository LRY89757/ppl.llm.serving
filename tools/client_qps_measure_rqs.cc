#include <iostream>
#include <chrono>
#include <map>
#include <fstream>
#include <vector>
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

int main() {
    // Define and initialize your data structures
    std::map<std::string, rapidjson::Value> throughput_dict;
    std::vector<double> request_rates = {1, 5, 10, 20, 50, 100, 200, 0}; // Example request rates

    // Create a RapidJSON document for the final output
    rapidjson::Document json_throughput_dict;
    json_throughput_dict.SetObject();

    // Create a RapidJSON array for the request rates
    rapidjson::Value request_rate_array(rapidjson::kArrayType);

    // Loop through request rates
    for (double request_rate : request_rates) {
        // Perform calculations similar to Python code here
        // Replace this with your actual benchmarking logic

        double total_time = 0;  // Replace with your actual values
        double throughput = 0; // Replace with your actual values
        double avg_latency = 0; // Replace with your actual values
        double avg_per_token_latency = 0; // Replace with your actual values
        double avg_per_output_token_latency = 0; // Replace with your actual values

        // Create a RapidJSON object for the metrics
        rapidjson::Value metrics_object(rapidjson::kObjectType);
        metrics_object.AddMember("total_time", rapidjson::Value(total_time), json_throughput_dict.GetAllocator());
        metrics_object.AddMember("throughput", rapidjson::Value(throughput), json_throughput_dict.GetAllocator());
        metrics_object.AddMember("avg_latency", rapidjson::Value(avg_latency), json_throughput_dict.GetAllocator());
        metrics_object.AddMember("avg_per_token_latency", rapidjson::Value(avg_per_token_latency), json_throughput_dict.GetAllocator());
        metrics_object.AddMember("avg_per_output_token_latency", rapidjson::Value(avg_per_output_token_latency), json_throughput_dict.GetAllocator());

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
    std::ofstream json_file("/home/ubuntu/projects/ppl.llm.serving/throughput.json");
    json_file << buffer.GetString();
    json_file.close();

    return 0;
}
