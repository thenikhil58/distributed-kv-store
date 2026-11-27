#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "../runtime_config.h"

TEST(RuntimeConfig, ParsesValuesAndPeerLists)
{
    const auto path = std::filesystem::temp_directory_path() / "kvstore-runtime-test.conf";
    {
        std::ofstream output(path);
        output << "# node settings\n"
               << "port = 50051\n"
               << "peers = localhost:50052, localhost:50053\n"
               << "data_root = ./state\n"
               << "peers_file = peers.txt\n"
               << "auth_file = auth.conf\n"
               << "token_file = node.token\n"
               << "bind = 127.0.0.1\n"
               << "metrics_port = 60051\n"
               << "metrics_bind = 127.0.0.1\n"
               << "ca = ca.pem\n"
               << "cert = node.pem\n"
               << "key = node.key\n";
    }

    const auto config = LoadRuntimeConfig(path);
    EXPECT_EQ(config.port, "50051");
    ASSERT_EQ(config.peers.size(), 2U);
    EXPECT_EQ(config.peers[0], "localhost:50052");
    EXPECT_EQ(config.peers[1], "localhost:50053");
    EXPECT_EQ(config.data_root, "./state");
    EXPECT_EQ(config.peers_file, "peers.txt");
    EXPECT_EQ(config.auth_file, "auth.conf");
    EXPECT_EQ(config.token_file, "node.token");
    EXPECT_EQ(config.bind, "127.0.0.1");
    EXPECT_EQ(config.metrics_port, "60051");
    EXPECT_EQ(config.metrics_bind, "127.0.0.1");
    EXPECT_EQ(config.ca_file, "ca.pem");
    EXPECT_EQ(config.certificate_file, "node.pem");
    EXPECT_EQ(config.private_key_file, "node.key");
    std::filesystem::remove(path);
}

TEST(RuntimeConfig, RejectsUnknownKeys)
{
    const auto path = std::filesystem::temp_directory_path() / "kvstore-runtime-invalid.conf";
    {
        std::ofstream output(path);
        output << "unsupported = value\n";
    }
    EXPECT_THROW(LoadRuntimeConfig(path), std::runtime_error);
    std::filesystem::remove(path);
}

TEST(RuntimeConfig, RejectsMissingFile)
{
    const auto path = std::filesystem::temp_directory_path() / "kvstore-runtime-missing.conf";
    std::filesystem::remove(path);
    EXPECT_THROW(LoadRuntimeConfig(path), std::runtime_error);
}

TEST(RuntimeConfig, RejectsMalformedLines)
{
    const auto path = std::filesystem::temp_directory_path() / "kvstore-runtime-malformed.conf";
    {
        std::ofstream output(path);
        output << "port 50051\n";
    }
    EXPECT_THROW(LoadRuntimeConfig(path), std::runtime_error);
    std::filesystem::remove(path);
}

TEST(RuntimeConfig, AcceptsEmptyPeerLists)
{
    const auto path = std::filesystem::temp_directory_path() / "kvstore-runtime-empty-peers.conf";
    {
        std::ofstream output(path);
        output << "port = 50051\n"
               << "peers =\n";
    }

    const auto config = LoadRuntimeConfig(path);
    EXPECT_EQ(config.port, "50051");
    EXPECT_TRUE(config.peers.empty());
    std::filesystem::remove(path);
}

TEST(RuntimeConfig, RejectsPartialTlsConfiguration)
{
    const auto path = std::filesystem::temp_directory_path() / "kvstore-runtime-tls.conf";
    {
        std::ofstream output(path);
        output << "ca = ca.pem\n"
               << "cert = node.pem\n";
    }
    EXPECT_THROW(LoadRuntimeConfig(path), std::runtime_error);
    std::filesystem::remove(path);
}

TEST(RuntimeConfig, RejectsPartialAuthorizationConfiguration)
{
    RuntimeConfig config;
    config.auth_file = "auth.conf";
    EXPECT_THROW(ValidateRuntimeConfig(config), std::runtime_error);
}

TEST(RuntimeConfig, RejectsInvalidNodePort)
{
    RuntimeConfig config;
    config.port = "65536";
    EXPECT_THROW(ValidateRuntimeConfig(config), std::runtime_error);
}

TEST(RuntimeConfig, RejectsDuplicatePeerPorts)
{
    RuntimeConfig config;
    config.port = "50051";
    config.peers = {"localhost:50052", "127.0.0.1:50052"};
    EXPECT_THROW(ValidateRuntimeConfig(config), std::runtime_error);
}

TEST(RuntimeConfig, RejectsSelfAsPeer)
{
    RuntimeConfig config;
    config.port = "50051";
    config.peers = {"localhost:50051"};
    EXPECT_THROW(ValidateRuntimeConfig(config), std::runtime_error);
}

TEST(RuntimeConfig, IgnoresInlineComments)
{
    const auto path = std::filesystem::temp_directory_path() / "kvstore-runtime-comments.conf";
    {
        std::ofstream output(path);
        output << "port = 50051 # public listener\n";
    }
    EXPECT_EQ(LoadRuntimeConfig(path).port, "50051");
    std::filesystem::remove(path);
}
