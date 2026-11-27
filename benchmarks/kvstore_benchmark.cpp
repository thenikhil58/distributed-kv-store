#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "../kvstore.grpc.pb.h"

namespace {

std::string ReadFile(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("failed to read credential file: " + path);
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool IsLoopback(const std::string& address) {
    const auto separator = address.rfind(':');
    const auto host = separator == std::string::npos ? address : address.substr(0, separator);
    return host == "localhost" || host == "127.0.0.1" || host == "::1" || host == "[::1]";
}

} // namespace

int main(int argc, char **argv)
{
    const std::string address = argc > 1 ? argv[1] : "localhost:50051";
    const int operations = argc > 2 ? std::atoi(argv[2]) : 1000;
    if (operations <= 0)
    {
        std::cerr << "operations must be positive\n";
        return 1;
    }

    std::string ca_file;
    std::string certificate_file;
    std::string private_key_file;
    std::string token_file;
    bool insecure_dev = false;
    for (int index = 3; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--ca" && index + 1 < argc) {
            ca_file = argv[++index];
        } else if (option == "--cert" && index + 1 < argc) {
            certificate_file = argv[++index];
        } else if (option == "--key" && index + 1 < argc) {
            private_key_file = argv[++index];
        } else if (option == "--token-file" && index + 1 < argc) {
            token_file = argv[++index];
        } else if (option == "--insecure-dev") {
            insecure_dev = true;
        } else {
            std::cerr << "invalid or incomplete option: " << option << '\n';
            return 2;
        }
    }

    const bool any_tls = !ca_file.empty() || !certificate_file.empty() || !private_key_file.empty();
    const bool complete_tls = !ca_file.empty() && !certificate_file.empty() && !private_key_file.empty();
    if (any_tls != complete_tls || complete_tls != !token_file.empty()) {
        std::cerr << "--ca, --cert, --key, and --token-file must be provided together\n";
        return 2;
    }
    if (insecure_dev && !IsLoopback(address)) {
        std::cerr << "--insecure-dev is restricted to loopback addresses\n";
        return 2;
    }
    if (!insecure_dev && !complete_tls) {
        std::cerr << "mTLS and --token-file are required; use --insecure-dev only for local development\n";
        return 2;
    }

    std::shared_ptr<grpc::ChannelCredentials> credentials;
    try {
        if (complete_tls) {
            grpc::SslCredentialsOptions options;
            options.pem_root_certs = ReadFile(ca_file);
            options.pem_cert_chain = ReadFile(certificate_file);
            options.pem_private_key = ReadFile(private_key_file);
            credentials = grpc::SslCredentials(options);
            const auto permissions = std::filesystem::status(token_file).permissions();
            if ((permissions & (std::filesystem::perms::group_all |
                                std::filesystem::perms::others_all)) != std::filesystem::perms::none) {
                throw std::runtime_error("token file must have mode 0600 or stricter");
            }
            auto token = ReadFile(token_file);
            while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) {
                token.pop_back();
            }
            if (token.size() < 32) {
                throw std::runtime_error("token file must contain at least 32 bytes");
            }
            credentials = grpc::CompositeChannelCredentials(
                credentials, grpc::AccessTokenCredentials(token));
        } else {
            credentials = grpc::InsecureChannelCredentials();
        }
    } catch (const std::exception& error) {
        std::cerr << "failed to load credentials: " << error.what() << '\n';
        return 2;
    }

    auto channel = grpc::CreateChannel(address, credentials);
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
