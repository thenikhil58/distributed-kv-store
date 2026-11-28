#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "../file_credentials.h"
#include "../kvstore.grpc.pb.h"

namespace {

bool IsLoopback(const std::string &address)
{
    const auto separator = address.rfind(':');
    const auto host = separator == std::string::npos ? address : address.substr(0, separator);
    return host == "localhost" || host == "127.0.0.1" || host == "::1" || host == "[::1]";
}

double Percentile(const std::vector<double> &sorted, double fraction)
{
    if (sorted.empty())
        return 0.0;
    const auto rank = static_cast<std::size_t>(std::ceil(fraction * sorted.size()));
    return sorted[std::max<std::size_t>(1, rank) - 1];
}

} // namespace

int main(int argc, char **argv)
{
    const std::string address = argc > 1 ? argv[1] : "localhost:50051";
    const int operations = argc > 2 ? std::atoi(argv[2]) : 1000;
    int workers = 1;
    int cluster_size = 1;
    int value_bytes = 16;
    std::string mode = "set";
    std::string key_prefix = "benchmark-";
    std::string ca_file;
    std::string certificate_file;
    std::string private_key_file;
    std::string token_file;
    std::string crl_directory;
    bool insecure_dev = false;

    for (int index = 3; index < argc; ++index)
    {
        const std::string option = argv[index];
        if (option == "--workers" && index + 1 < argc)
            workers = std::atoi(argv[++index]);
        else if (option == "--cluster-size" && index + 1 < argc)
            cluster_size = std::atoi(argv[++index]);
        else if (option == "--value-bytes" && index + 1 < argc)
            value_bytes = std::atoi(argv[++index]);
        else if (option == "--mode" && index + 1 < argc)
            mode = argv[++index];
        else if (option == "--key-prefix" && index + 1 < argc)
            key_prefix = argv[++index];
        else if (option == "--ca" && index + 1 < argc)
            ca_file = argv[++index];
        else if (option == "--cert" && index + 1 < argc)
            certificate_file = argv[++index];
        else if (option == "--key" && index + 1 < argc)
            private_key_file = argv[++index];
        else if (option == "--token-file" && index + 1 < argc)
            token_file = argv[++index];
        else if (option == "--crl-directory" && index + 1 < argc)
            crl_directory = argv[++index];
        else if (option == "--insecure-dev")
            insecure_dev = true;
        else
        {
            std::cerr << "invalid or incomplete option: " << option << '\n';
            return 2;
        }
    }

    if (operations <= 0 || operations > 10'000'000 || workers < 1 ||
        workers > 256 || cluster_size < 1 || cluster_size > 9 ||
        value_bytes < 0 || value_bytes > 1024 * 1024 ||
        key_prefix.size() + 16 > 4096 ||
        (mode != "set" && mode != "get" && mode != "mixed"))
    {
        std::cerr << "invalid operations, workers, cluster size, value size, key prefix, or mode\n";
        return 2;
    }

    const bool any_tls = !ca_file.empty() || !certificate_file.empty() ||
                         !private_key_file.empty();
    const bool complete_tls = !ca_file.empty() && !certificate_file.empty() &&
                              !private_key_file.empty();
    if (any_tls != complete_tls || complete_tls != !token_file.empty() ||
        (!complete_tls && !crl_directory.empty()))
    {
        std::cerr << "--ca, --cert, --key, and --token-file must be provided together; "
                     "--crl-directory requires TLS\n";
        return 2;
    }
    if (insecure_dev && !IsLoopback(address))
    {
        std::cerr << "--insecure-dev is restricted to loopback addresses\n";
        return 2;
    }
    if (!insecure_dev && !complete_tls)
    {
        std::cerr << "mTLS and --token-file are required; "
                     "use --insecure-dev only for local development\n";
        return 2;
    }

    std::shared_ptr<grpc::ChannelCredentials> credentials;
    try
    {
        if (complete_tls)
        {
            credentials = kvstore_credentials::CreateChannelCredentials(
                ca_file, certificate_file, private_key_file, crl_directory);
            credentials = grpc::CompositeChannelCredentials(
                credentials, kvstore_credentials::CreateFileTokenCredentials(token_file));
        }
        else
        {
            credentials = grpc::InsecureChannelCredentials();
        }
    }
    catch (const std::exception &error)
    {
        std::cerr << "failed to load credentials: " << error.what() << '\n';
        return 2;
    }

    auto channel = grpc::CreateChannel(address, credentials);
    auto stub = kvstore::KVStore::NewStub(channel);
    const std::string value(static_cast<std::size_t>(value_bytes), 'v');
    if (mode == "get")
    {
        for (int operation = 0; operation < operations; ++operation)
        {
            kvstore::SetRequest request;
            request.set_key(key_prefix + std::to_string(operation));
            request.set_value(value);
            kvstore::SetResponse response;
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() +
                                 std::chrono::seconds(10));
            const auto status = stub->Set(&context, request, &response);
            if (!status.ok() || !response.success())
            {
                std::cerr << "GET-mode setup write failed at operation " << operation
                          << ": " << status.error_message() << '\n';
                return 1;
            }
        }
    }
    std::atomic<int> next_operation{0};
    std::atomic<int> succeeded{0};
    std::atomic<int> failed{0};
    std::atomic<bool> start{false};
    std::mutex error_mutex;
    std::string first_error;
    std::vector<std::vector<double>> worker_latencies(static_cast<std::size_t>(workers));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(workers));

    for (int worker = 0; worker < workers; ++worker)
    {
        worker_latencies[static_cast<std::size_t>(worker)].reserve(
            static_cast<std::size_t>(operations / workers + 1));
        threads.emplace_back([&, worker]() {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            auto &latencies = worker_latencies[static_cast<std::size_t>(worker)];
            while (true)
            {
                const int operation = next_operation.fetch_add(1);
                if (operation >= operations)
                    break;
                const bool write = mode == "set" || (mode == "mixed" && operation % 2 == 0);
                const int key_index = mode == "mixed" ? operation / 2 : operation;
                const std::string key = key_prefix + std::to_string(key_index);
                const auto operation_started = std::chrono::steady_clock::now();
                grpc::Status status;
                if (write)
                {
                    kvstore::SetRequest request;
                    request.set_key(key);
                    request.set_value(value);
                    kvstore::SetResponse response;
                    grpc::ClientContext context;
                    context.set_deadline(std::chrono::system_clock::now() +
                                         std::chrono::seconds(10));
                    status = stub->Set(&context, request, &response);
                    if (status.ok() && !response.success())
                        status = grpc::Status(grpc::StatusCode::ABORTED, "write was not committed");
                }
                else
                {
                    kvstore::GetRequest request;
                    request.set_key(key);
                    kvstore::GetResponse response;
                    grpc::ClientContext context;
                    context.set_deadline(std::chrono::system_clock::now() +
                                         std::chrono::seconds(10));
                    status = stub->Get(&context, request, &response);
                }
                latencies.push_back(std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - operation_started).count());
                if (status.ok())
                {
                    ++succeeded;
                }
                else
                {
                    ++failed;
                    std::lock_guard<std::mutex> lock(error_mutex);
                    if (first_error.empty())
                        first_error = status.error_message();
                }
            }
        });
    }

    const auto started = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    for (auto &thread : threads)
        thread.join();
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();

    std::vector<double> latencies;
    latencies.reserve(static_cast<std::size_t>(operations));
    for (auto &worker_values : worker_latencies)
        latencies.insert(latencies.end(), worker_values.begin(), worker_values.end());
    std::sort(latencies.begin(), latencies.end());

    std::cout << std::fixed << std::setprecision(3)
              << "{\"mode\":\"" << mode << "\",\"cluster_size\":" << cluster_size
              << ",\"workers\":" << workers
              << ",\"value_bytes\":" << value_bytes
              << ",\"operations\":" << operations
              << ",\"succeeded\":" << succeeded.load()
              << ",\"failed\":" << failed.load()
              << ",\"elapsed_seconds\":" << elapsed
              << ",\"ops_per_second\":" << succeeded.load() / elapsed
              << ",\"p50_ms\":" << Percentile(latencies, 0.50)
              << ",\"p95_ms\":" << Percentile(latencies, 0.95)
              << ",\"p99_ms\":" << Percentile(latencies, 0.99)
              << ",\"max_ms\":" << (latencies.empty() ? 0.0 : latencies.back())
              << "}\n";
    if (!first_error.empty())
        std::cerr << "first failed operation: " << first_error << '\n';
    return failed.load() == 0 ? 0 : 1;
}
