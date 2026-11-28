#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct RuntimeConfig {
    std::string port;
    std::string bind = "127.0.0.1";
    std::string metrics_port;
    std::string metrics_bind = "127.0.0.1";
    std::vector<std::string> peers;
    std::filesystem::path data_root = "data";
    std::filesystem::path peers_file;
    std::filesystem::path auth_file;
    std::filesystem::path token_file;
    std::filesystem::path crl_directory;
    std::string ca_file;
    std::string certificate_file;
    std::string private_key_file;
};

RuntimeConfig LoadRuntimeConfig(const std::filesystem::path &path);
int ParsePortNumber(const std::string &value, const std::string &field);
void ValidateRuntimeConfig(const RuntimeConfig &config);
