#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "../kvstore.grpc.pb.h"

int main(int argc, char **argv)
{
    const std::string address = argc > 1 ? argv[1] : "localhost:50051";
    const int operations = argc > 2 ? std::atoi(argv[2]) : 1000;
    if (operations <= 0)
    {
        std::cerr << "operations must be positive\n";
        return 1;
    }

    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    auto stub = kvstore::KVStore::NewStub(channel);
    std::vector<double> latencies;
    latencies.reserve(static_cast<std::size_t>(operations));
    const auto started = std::chrono::steady_clock::now();
    for (int index = 0; index < operations; ++index)
    {
        const auto operation_started = std::chrono::steady_clock::now();
        kvstore::SetRequest request;
        request.set_key("benchmark-" + std::to_string(index));
        request.set_value("value");
        kvstore::SetResponse response;
        grpc::ClientContext context;
        const auto status = stub->Set(&context, request, &response);
        if (!status.ok() || !response.success())
        {
            std::cerr << "Set failed at operation " << index << ": "
                      << status.error_message() << "\n";
            return 1;
        }
        latencies.push_back(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - operation_started)
                .count());
    }

    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::sort(latencies.begin(), latencies.end());
    const auto percentile = [&](double fraction) {
        const auto index =
            std::min(latencies.size() - 1,
                     static_cast<std::size_t>(
                         fraction * static_cast<double>(latencies.size())));
        return latencies[index];
    };
    std::cout << "operations=" << operations << " seconds=" << elapsed
              << " ops_per_second=" << (operations / elapsed)
              << " p50_ms=" << percentile(0.50)
              << " p95_ms=" << percentile(0.95)
              << " p99_ms=" << percentile(0.99)
              << " max_ms=" << latencies.back() << '\n';
    return 0;
}
