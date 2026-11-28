#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/security/tls_certificate_provider.h>
#include <grpcpp/security/tls_credentials_options.h>

namespace kvstore_credentials {

inline std::string ReadPrivateToken(const std::filesystem::path &path)
{
    const auto permissions = std::filesystem::status(path).permissions();
    if ((permissions & (std::filesystem::perms::group_all |
                       std::filesystem::perms::others_all)) != std::filesystem::perms::none)
        throw std::runtime_error("token file must have mode 0600 or stricter: " + path.string());
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("failed to read token file: " + path.string());
    std::string token{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r'))
        token.pop_back();
    if (token.size() < 32)
        throw std::runtime_error("token file must contain at least 32 bytes");
    return token;
}

inline std::shared_ptr<grpc::experimental::FileWatcherCertificateProvider>
CreateCertificateProvider(const std::string &ca_file, const std::string &certificate_file,
                          const std::string &private_key_file)
{
    return std::make_shared<grpc::experimental::FileWatcherCertificateProvider>(
        private_key_file, certificate_file, ca_file, 5);
}

inline std::shared_ptr<grpc::ChannelCredentials>
CreateChannelCredentials(const std::string &ca_file, const std::string &certificate_file,
                         const std::string &private_key_file, const std::string &crl_directory)
{
    auto provider = CreateCertificateProvider(ca_file, certificate_file, private_key_file);
    grpc::experimental::TlsChannelCredentialsOptions options;
    options.set_certificate_provider(provider);
    options.watch_root_certs();
    options.watch_identity_key_cert_pairs();
    if (!crl_directory.empty())
        options.set_crl_directory(crl_directory);
    return grpc::experimental::TlsCredentials(options);
}

inline std::shared_ptr<grpc::ServerCredentials>
CreateServerCredentials(const std::string &ca_file, const std::string &certificate_file,
                        const std::string &private_key_file, const std::string &crl_directory)
{
    auto provider = CreateCertificateProvider(ca_file, certificate_file, private_key_file);
    grpc::experimental::TlsServerCredentialsOptions options(provider);
    options.watch_root_certs();
    options.watch_identity_key_cert_pairs();
    options.set_cert_request_type(
        GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY);
    if (!crl_directory.empty())
        options.set_crl_directory(crl_directory);
    return grpc::experimental::TlsServerCredentials(options);
}

inline std::shared_ptr<grpc::CallCredentials>
CreateFileTokenCredentials(const std::filesystem::path &path)
{
    class TokenFilePlugin final : public grpc::MetadataCredentialsPlugin
    {
    public:
        explicit TokenFilePlugin(std::filesystem::path token_path)
            : token_path_(std::move(token_path))
        {
        }

        grpc::Status GetMetadata(grpc::string_ref, grpc::string_ref,
                                 const grpc::AuthContext &,
                                 std::multimap<grpc::string, grpc::string> *metadata) override
        {
            try
            {
                metadata->emplace("authorization", "Bearer " + ReadPrivateToken(token_path_));
                return grpc::Status::OK;
            }
            catch (const std::exception &error)
            {
                return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, error.what());
            }
        }

    private:
        std::filesystem::path token_path_;
    };

    return grpc::MetadataCredentialsFromPlugin(
        std::make_unique<TokenFilePlugin>(path));
}

} // namespace kvstore_credentials
