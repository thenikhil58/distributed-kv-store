#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <string>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <openssl/sha.h>

#include "../kvstore.grpc.pb.h"

namespace {

std::string ResolveServerBinary() {
    std::vector<std::filesystem::path> candidates = {
        std::filesystem::current_path() / "server",
        std::filesystem::current_path() / "build" / "server",
        std::filesystem::current_path().parent_path() / "build" / "server",
        std::filesystem::current_path() / "build" / "DistributedKVStore" / "server",
        std::filesystem::current_path() / "build" / "src" / "server"
    };

    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate.string();
        }
    }

    return "server";
}

bool WaitForServer(const std::string& address, int timeout_seconds = 20) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    kvstore::KVStore::Stub stub(channel);
    while (std::chrono::steady_clock::now() < deadline) {
        kvstore::ClusterStatusRequest req;
        kvstore::ClusterStatusResponse resp;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(1));
        const auto status = stub.GetClusterStatus(&context, req, &resp);
        if (status.ok()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

bool WaitForLeader(const std::string& address, int timeout_seconds = 10) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    kvstore::KVStore::Stub stub(channel);
    while (std::chrono::steady_clock::now() < deadline) {
        kvstore::SetRequest req;
        req.set_key("__raft_test_readiness__");
        req.set_value("ready");
        kvstore::SetResponse resp;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(1));
        const auto status = stub.Set(&context, req, &resp);
        if (status.ok() && resp.success()) {
            kvstore::DeleteRequest delete_req;
            delete_req.set_key("__raft_test_readiness__");
            kvstore::DeleteResponse delete_resp;
            grpc::ClientContext delete_context;
            delete_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(1));
            stub.Delete(&delete_context, delete_req, &delete_resp);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

pid_t StartServer(const std::string& port, const std::vector<std::string>& peers,
                  const std::vector<std::string>& options = {}) {
    std::vector<std::string> argv;
    argv.push_back(ResolveServerBinary());
    argv.push_back(port);
    for (const auto& peer : peers) {
        argv.push_back(peer);
    }
    argv.push_back("--insecure-dev");
    argv.insert(argv.end(), options.begin(), options.end());

    std::vector<char*> cargs;
    for (auto& arg : argv) {
        cargs.push_back(arg.data());
    }

    std::string Sha256HexForTest(const std::string& value) {
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
        std::ostringstream result;
        for (const auto byte : digest) {
            result << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
        }
        return result.str();
    }
    cargs.push_back(nullptr);

    pid_t pid = fork();
    if (pid == 0) {
        execv(cargs[0], cargs.data());
        std::perror("execv server");
        std::_Exit(1);
    }
    return pid;
}

void StopServer(pid_t pid) {
    if (pid > 0) {
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
    }
}

struct TestServer {
    explicit TestServer(const std::string& port, bool reset_storage = true,
                        const std::vector<std::string>& options = {})
        : port(port), pid(-1) {
        if (reset_storage) {
            std::error_code error;
            std::filesystem::remove_all(std::filesystem::path("data") / ("node_" + port), error);
        }
        pid = StartServer(port, {}, options);
    }

    ~TestServer() {
        StopServer(pid);
    }

    std::string port;
    pid_t pid;
};

struct TestCluster {
    explicit TestCluster(const std::vector<std::string>& ports) {
        for (const auto& port : ports) {
            std::error_code error;
            std::filesystem::remove_all(std::filesystem::path("data") / ("node_" + port), error);
        }
        for (const auto& port : ports) {
            std::vector<std::string> peers;
            for (const auto& peer_port : ports) {
                if (peer_port != port) {
                    peers.push_back("localhost:" + peer_port);
                }
            }
            pids.push_back(StartServer(port, peers));
        }
    }

    ~TestCluster() {
        for (const auto pid : pids) {
            StopServer(pid);
        }
    }

    std::vector<pid_t> pids;
};

TEST(RaftIntegration, LeaderSetAndGetRoundTrip) {
    const std::string port = "18051";
    const std::string address = "localhost:" + port;
    TestServer server(port);
    ASSERT_GT(server.pid, 0);
    ASSERT_TRUE(WaitForServer(address));
    ASSERT_TRUE(WaitForLeader(address));

    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    kvstore::KVStore::Stub stub(channel);

    kvstore::SetRequest set_req;
    set_req.set_key("alpha");
    set_req.set_value("beta");
    kvstore::SetResponse set_resp;
    grpc::ClientContext set_ctx;
    auto set_status = stub.Set(&set_ctx, set_req, &set_resp);
    ASSERT_TRUE(set_status.ok());
    ASSERT_TRUE(set_resp.success());

    kvstore::GetRequest get_req;
    get_req.set_key("alpha");
    kvstore::GetResponse get_resp;
    grpc::ClientContext get_ctx;
    auto get_status = stub.Get(&get_ctx, get_req, &get_resp);
    ASSERT_TRUE(get_status.ok());
    ASSERT_TRUE(get_resp.found());
    EXPECT_EQ(get_resp.value(), "beta");

}

TEST(RaftIntegration, RpcTokensEnforceAuthenticationAndLeastPrivilege) {
    const std::string read_token = "read-token-for-test-0123456789-abcdefgh";
    const std::string write_token = "write-token-for-test-0123456789-abcdefg";
    const std::string monitor_token = "monitor-token-test-0123456789-abcdef";
    const auto directory = std::filesystem::temp_directory_path() /
                           ("kvstore-grpc-auth-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    const auto auth_path = directory / "authorization.conf";
    const auto token_path = directory / "node-token";
    {
        std::ofstream auth(auth_path);
        auth << Sha256HexForTest(read_token) << "=read\n"
             << Sha256HexForTest(write_token) << "=write\n"
             << Sha256HexForTest(monitor_token) << "=monitor\n";
        std::ofstream node_token(token_path);
        node_token << monitor_token << '\n';
    }
    ASSERT_EQ(chmod(auth_path.c_str(), 0600), 0);
    ASSERT_EQ(chmod(token_path.c_str(), 0600), 0);

    TestServer server("18100", true, {
        "--auth-file", auth_path.string(),
        "--token-file", token_path.string(),
    });
    ASSERT_GT(server.pid, 0);

    const auto address = "localhost:" + server.port;
    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    kvstore::KVStore::Stub stub(channel);
    auto call_status = [&](const std::string& token) {
        grpc::ClientContext context;
        if (!token.empty()) {
            context.AddMetadata("authorization", "Bearer " + token);
        }
        kvstore::ClusterStatusRequest request;
        kvstore::ClusterStatusResponse response;
        return stub.GetClusterStatus(&context, request, &response);
    };

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    grpc::Status monitor_status;
    while (std::chrono::steady_clock::now() < deadline) {
        monitor_status = call_status(monitor_token);
        if (monitor_status.ok()) {
            kvstore::ClusterStatusRequest request;
            kvstore::ClusterStatusResponse response;
            grpc::ClientContext context;
            context.AddMetadata("authorization", "Bearer " + monitor_token);
            if (stub.GetClusterStatus(&context, request, &response).ok() && response.is_leader()) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(monitor_status.ok());

    EXPECT_EQ(call_status("").error_code(), grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(call_status("unknown-token-value-0123456789-abcdefgh").error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(call_status(read_token).error_code(), grpc::StatusCode::PERMISSION_DENIED);

    kvstore::GetRequest get_request;
    get_request.set_key("rbac-key");
    kvstore::GetResponse get_response;
    grpc::ClientContext read_context;
    read_context.AddMetadata("authorization", "Bearer " + read_token);
    EXPECT_TRUE(stub.Get(&read_context, get_request, &get_response).ok());
    EXPECT_FALSE(get_response.found());

    kvstore::SetRequest set_request;
    set_request.set_key("rbac-key");
    set_request.set_value("value");
    kvstore::SetResponse set_response;
    grpc::ClientContext denied_write_context;
    denied_write_context.AddMetadata("authorization", "Bearer " + read_token);
    EXPECT_EQ(stub.Set(&denied_write_context, set_request, &set_response).error_code(),
              grpc::StatusCode::PERMISSION_DENIED);

    grpc::ClientContext write_context;
    write_context.AddMetadata("authorization", "Bearer " + write_token);
    ASSERT_TRUE(stub.Set(&write_context, set_request, &set_response).ok());
    ASSERT_TRUE(set_response.success());

    kvstore::GetResponse denied_get_response;
    grpc::ClientContext denied_read_context;
    denied_read_context.AddMetadata("authorization", "Bearer " + write_token);
    EXPECT_EQ(stub.Get(&denied_read_context, get_request, &denied_get_response).error_code(),
              grpc::StatusCode::PERMISSION_DENIED);

    StopServer(server.pid);
    server.pid = -1;
    std::error_code remove_error;
    std::filesystem::remove_all(directory, remove_error);
    EXPECT_FALSE(remove_error);
}

TEST(RaftIntegration, RejectsOversizedGrpcMessages) {
    TestServer server("18101");
    ASSERT_GT(server.pid, 0);
    ASSERT_TRUE(WaitForServer("localhost:" + server.port));

    grpc::ChannelArguments arguments;
    arguments.SetMaxSendMessageSize(20 * 1024 * 1024);
    auto channel = grpc::CreateCustomChannel(
        "localhost:" + server.port, grpc::InsecureChannelCredentials(), arguments);
    kvstore::KVStore::Stub stub(channel);
    kvstore::InstallSnapshotRequest request;
    request.set_leader_term(1);
    request.set_leader_id(std::stoi(server.port));
    request.set_last_included_index(1);
    request.set_last_included_term(1);
    request.set_snapshot_data(std::string(17 * 1024 * 1024, 'x'));
    kvstore::InstallSnapshotResponse response;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    EXPECT_EQ(stub.InstallSnapshot(&context, request, &response).error_code(),
              grpc::StatusCode::RESOURCE_EXHAUSTED);
}

TEST(RaftIntegration, LeaderCommitsNoOpBeforeReportingReadiness) {
    const std::string port = "18070";
    const std::string address = "localhost:" + port;
    TestServer server(port);
    ASSERT_GT(server.pid, 0);
    ASSERT_TRUE(WaitForServer(address));

    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    kvstore::KVStore::Stub stub(channel);
    kvstore::ClusterStatusResponse status_response;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        kvstore::ClusterStatusRequest status_request;
        grpc::ClientContext status_context;
        status_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(1));
        ASSERT_TRUE(stub.GetClusterStatus(&status_context, status_request, &status_response).ok());
        if (status_response.is_leader()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(status_response.is_leader());
    EXPECT_GE(status_response.commit_index(), 0);

    kvstore::GetRequest get_request;
    kvstore::GetResponse get_response;
    grpc::ClientContext get_context;
    ASSERT_TRUE(stub.Get(&get_context, get_request, &get_response).ok());
    EXPECT_FALSE(get_response.found());

    kvstore::VoteRequest pre_vote;
    pre_vote.set_candidate_term(status_response.current_term() + 1);
    pre_vote.set_candidate_id(18071);
    pre_vote.set_candidate_last_log_index(status_response.commit_index());
    pre_vote.set_candidate_last_log_term(status_response.current_term());
    pre_vote.set_pre_vote(true);
    kvstore::VoteResponse pre_vote_response;
    grpc::ClientContext vote_context;
    ASSERT_TRUE(stub.RequestVote(&vote_context, pre_vote, &pre_vote_response).ok());
    EXPECT_FALSE(pre_vote_response.vote_granted());
    EXPECT_EQ(pre_vote_response.voter_term(), status_response.current_term());
}

TEST(RaftIntegration, DeleteRemovesKey) {
    const std::string port = "18052";
    const std::string address = "localhost:" + port;
    TestServer server(port);
    ASSERT_GT(server.pid, 0);
    ASSERT_TRUE(WaitForServer(address));
    ASSERT_TRUE(WaitForLeader(address));

    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    kvstore::KVStore::Stub stub(channel);

    kvstore::SetRequest set_req;
    set_req.set_key("gamma");
    set_req.set_value("delta");
    kvstore::SetResponse set_resp;
    grpc::ClientContext set_ctx;
    auto set_status = stub.Set(&set_ctx, set_req, &set_resp);
    ASSERT_TRUE(set_status.ok());
    ASSERT_TRUE(set_resp.success());

    kvstore::DeleteRequest del_req;
    del_req.set_key("gamma");
    kvstore::DeleteResponse del_resp;
    grpc::ClientContext del_ctx;
    auto del_status = stub.Delete(&del_ctx, del_req, &del_resp);
    ASSERT_TRUE(del_status.ok());
    ASSERT_TRUE(del_resp.success());

    kvstore::GetRequest get_req;
    get_req.set_key("gamma");
    kvstore::GetResponse get_resp;
    grpc::ClientContext get_ctx;
    auto get_status = stub.Get(&get_ctx, get_req, &get_resp);
    ASSERT_TRUE(get_status.ok());
    EXPECT_FALSE(get_resp.found());

}

TEST(RaftIntegration, ListsKeysAndReportsClusterStatus) {
    const std::string port = "18053";
    const std::string address = "localhost:" + port;
    TestServer server(port);
    ASSERT_GT(server.pid, 0);
    ASSERT_TRUE(WaitForServer(address));
    ASSERT_TRUE(WaitForLeader(address));

    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    kvstore::KVStore::Stub stub(channel);

    for (const auto& entry : {std::pair{"first", "one"}, std::pair{"second", "two"}}) {
        kvstore::SetRequest set_req;
        set_req.set_key(entry.first);
        set_req.set_value(entry.second);
        kvstore::SetResponse set_resp;
        grpc::ClientContext set_ctx;
        const auto set_status = stub.Set(&set_ctx, set_req, &set_resp);
        ASSERT_TRUE(set_status.ok());
        ASSERT_TRUE(set_resp.success());
    }

    kvstore::ListKeysRequest list_req;
    kvstore::ListKeysResponse list_resp;
    grpc::ClientContext list_ctx;
    const auto list_status = stub.ListKeys(&list_ctx, list_req, &list_resp);
    ASSERT_TRUE(list_status.ok());
    ASSERT_EQ(list_resp.keys_size(), 2);
    EXPECT_EQ(list_resp.leader_id(), std::stoi(port));
    EXPECT_NE(std::find(list_resp.keys().begin(), list_resp.keys().end(), "first"), list_resp.keys().end());
    EXPECT_NE(std::find(list_resp.keys().begin(), list_resp.keys().end(), "second"), list_resp.keys().end());

    kvstore::ClusterStatusRequest status_req;
    kvstore::ClusterStatusResponse status_resp;
    grpc::ClientContext status_ctx;
    const auto status = stub.GetClusterStatus(&status_ctx, status_req, &status_resp);
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(status_resp.node_id(), std::stoi(port));
    EXPECT_EQ(status_resp.leader_id(), std::stoi(port));
    EXPECT_GE(status_resp.current_term(), 0);
    EXPECT_GE(status_resp.commit_index(), 2);
    EXPECT_TRUE(status_resp.is_leader());

}

TEST(RaftIntegration, RecoversCommittedValueAfterRestart) {
    const std::string port = "18054";
    const std::string address = "localhost:" + port;

    {
        TestServer server(port);
        ASSERT_GT(server.pid, 0);
        ASSERT_TRUE(WaitForServer(address));
        ASSERT_TRUE(WaitForLeader(address));

        auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
        kvstore::KVStore::Stub stub(channel);
        kvstore::SetRequest request;
        request.set_key("persistent-key");
        request.set_value("persistent-value");
        request.set_request_id("persisted-write-1");
        kvstore::SetResponse response;
        grpc::ClientContext context;
        ASSERT_TRUE(stub.Set(&context, request, &response).ok());
        ASSERT_TRUE(response.success());
    }

    TestServer restarted(port, false);
    ASSERT_GT(restarted.pid, 0);
    ASSERT_TRUE(WaitForServer(address));
    ASSERT_TRUE(WaitForLeader(address));

    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    kvstore::KVStore::Stub stub(channel);
    kvstore::GetRequest request;
    request.set_key("persistent-key");
    kvstore::GetResponse response;
    grpc::ClientContext context;
    ASSERT_TRUE(stub.Get(&context, request, &response).ok());
    ASSERT_TRUE(response.found());
    EXPECT_EQ(response.value(), "persistent-value");

    kvstore::SetRequest retry_request;
    retry_request.set_key("persistent-key");
    retry_request.set_value("must-not-overwrite");
    retry_request.set_request_id("persisted-write-1");
    kvstore::SetResponse retry_response;
    grpc::ClientContext retry_context;
    ASSERT_TRUE(stub.Set(&retry_context, retry_request, &retry_response).ok());
    ASSERT_TRUE(retry_response.success());

    kvstore::GetRequest verify_request;
    verify_request.set_key("persistent-key");
    kvstore::GetResponse after_retry;
    grpc::ClientContext after_retry_context;
    ASSERT_TRUE(stub.Get(&after_retry_context, verify_request, &after_retry).ok());
    ASSERT_TRUE(after_retry.found());
    EXPECT_EQ(after_retry.value(), "persistent-value");
}

TEST(RaftIntegration, FollowerForwardsWritesToLeader) {
    const std::string leader_port = "18055";
    const std::string follower_port = "18056";
    const std::string third_port = "18057";

    auto leader_address = "localhost:" + leader_port;
    auto follower_address = "localhost:" + follower_port;

    TestCluster cluster({leader_port, follower_port, third_port});

    ASSERT_TRUE(WaitForServer(leader_address));
    ASSERT_TRUE(WaitForServer(follower_address));
    ASSERT_TRUE(WaitForServer("localhost:" + third_port));

    std::string leader_seen;
    for (int attempt = 0; attempt < 50; ++attempt) {
        for (const auto &address : {leader_address, follower_address, "localhost:" + third_port}) {
            auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
            kvstore::KVStore::Stub stub(channel);
            kvstore::ClusterStatusRequest status_req;
            kvstore::ClusterStatusResponse status_resp;
            grpc::ClientContext status_ctx;
            status_ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(1));
            const auto status = stub.GetClusterStatus(&status_ctx, status_req, &status_resp);
            if (status.ok() && status_resp.is_leader()) {
                leader_seen = address;
                break;
            }
        }
        if (!leader_seen.empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    ASSERT_FALSE(leader_seen.empty());

    const auto follower_address_without_leader = leader_seen == leader_address ? follower_address : leader_address;
    auto channel = grpc::CreateChannel(follower_address_without_leader, grpc::InsecureChannelCredentials());
    kvstore::KVStore::Stub stub(channel);

    kvstore::SetRequest request;
    request.set_key("forwarded-from-follower");
    request.set_value("ok");
    kvstore::SetResponse response;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    ASSERT_TRUE(stub.Set(&context, request, &response).ok());
    ASSERT_TRUE(response.success());

    auto leader_channel = grpc::CreateChannel(leader_seen, grpc::InsecureChannelCredentials());
    kvstore::KVStore::Stub leader_stub(leader_channel);
    kvstore::GetRequest get_req;
    get_req.set_key("forwarded-from-follower");
    kvstore::GetResponse get_resp;
    grpc::ClientContext get_ctx;
    get_ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    ASSERT_TRUE(leader_stub.Get(&get_ctx, get_req, &get_resp).ok());
    ASSERT_TRUE(get_resp.found());
    EXPECT_EQ(get_resp.value(), "ok");

}

TEST(RaftIntegration, FollowerRepliesIncludeLeaderHint) {
    const std::vector<std::string> ports = {"18066", "18067", "18068"};
    for (const auto& port : ports) {
        std::error_code error;
        std::filesystem::remove_all(std::filesystem::path("data") / ("node_" + port), error);
    }

    TestCluster cluster(ports);
    for (const auto& port : ports) {
        ASSERT_TRUE(WaitForServer("localhost:" + port));
    }
    ASSERT_TRUE(WaitForLeader("localhost:" + ports.front()));

    std::string leader_port;
    std::string follower_port;
    for (const auto& port : ports) {
        auto channel = grpc::CreateChannel("localhost:" + port, grpc::InsecureChannelCredentials());
        kvstore::KVStore::Stub stub(channel);
        kvstore::ClusterStatusRequest request;
        kvstore::ClusterStatusResponse response;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        ASSERT_TRUE(stub.GetClusterStatus(&context, request, &response).ok());
        if (response.is_leader()) {
            leader_port = port;
        } else if (follower_port.empty()) {
            follower_port = port;
        }
    }
    ASSERT_FALSE(leader_port.empty());
    ASSERT_FALSE(follower_port.empty());

    auto follower_channel = grpc::CreateChannel("localhost:" + follower_port, grpc::InsecureChannelCredentials());
    kvstore::KVStore::Stub follower_stub(follower_channel);

    kvstore::GetRequest get_request;
    get_request.set_key("leader-hint-read");
    kvstore::GetResponse get_response;
    grpc::ClientContext get_context;
    ASSERT_TRUE(follower_stub.Get(&get_context, get_request, &get_response).ok());
    EXPECT_EQ(get_response.leader_id(), std::stoi(leader_port));

    kvstore::SetRequest set_request;
    set_request.set_key("leader-hint-write");
    set_request.set_value("value");
    kvstore::SetResponse set_response;
    grpc::ClientContext set_context;
    ASSERT_TRUE(follower_stub.Set(&set_context, set_request, &set_response).ok());
    EXPECT_TRUE(set_response.success());
    EXPECT_EQ(set_response.leader_id(), std::stoi(leader_port));
}

TEST(RaftIntegration, DuplicateWriteRequestsWithSameIdAreIdempotent) {
    const std::string port = "18069";
    const std::string address = "localhost:" + port;
    TestServer server(port);
    ASSERT_GT(server.pid, 0);
    ASSERT_TRUE(WaitForServer(address));
    ASSERT_TRUE(WaitForLeader(address));

    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    kvstore::KVStore::Stub stub(channel);

    kvstore::SetRequest first;
    first.set_key("idempotent-key");
    first.set_value("first-value");
    first.set_request_id("retry-1");
    kvstore::SetResponse first_response;
    grpc::ClientContext first_context;
    ASSERT_TRUE(stub.Set(&first_context, first, &first_response).ok());
    ASSERT_TRUE(first_response.success());

    kvstore::SetRequest duplicate;
    duplicate.set_key("idempotent-key");
    duplicate.set_value("second-value");
    duplicate.set_request_id("retry-1");
    kvstore::SetResponse duplicate_response;
    grpc::ClientContext duplicate_context;
    ASSERT_TRUE(stub.Set(&duplicate_context, duplicate, &duplicate_response).ok());
    ASSERT_TRUE(duplicate_response.success());

    kvstore::GetRequest get_request;
    get_request.set_key("idempotent-key");
    kvstore::GetResponse get_response;
    grpc::ClientContext get_context;
    ASSERT_TRUE(stub.Get(&get_context, get_request, &get_response).ok());
    ASSERT_TRUE(get_response.found());
    EXPECT_EQ(get_response.value(), "first-value");
}

TEST(RaftIntegration, RejectsVoteFromCandidateWithStaleLog) {
    const std::vector<std::string> ports = {"18060", "18061", "18062"};
    for (const auto& port : ports) {
        std::error_code error;
        std::filesystem::remove_all(std::filesystem::path("data") / ("node_" + port), error);
    }
    TestCluster cluster(ports);
    for (const auto& port : ports) {
        ASSERT_TRUE(WaitForServer("localhost:" + port));
    }
    ASSERT_TRUE(WaitForLeader("localhost:" + ports.front()));

    std::string voter_address;
    kvstore::ClusterStatusResponse voter_status;
    for (const auto& port : ports) {
        const auto address = "localhost:" + port;
        auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
        kvstore::KVStore::Stub stub(channel);
        kvstore::ClusterStatusRequest status_request;
        kvstore::ClusterStatusResponse status_response;
        grpc::ClientContext status_context;
        ASSERT_TRUE(stub.GetClusterStatus(&status_context, status_request, &status_response).ok());
        if (!status_response.is_leader()) {
            voter_address = address;
            voter_status = status_response;
            break;
        }
    }
    ASSERT_FALSE(voter_address.empty());
    ASSERT_GE(voter_status.commit_index(), 0);

    auto voter_channel = grpc::CreateChannel(voter_address, grpc::InsecureChannelCredentials());
    kvstore::KVStore::Stub voter_stub(voter_channel);
    kvstore::VoteRequest vote_request;
    vote_request.set_candidate_term(voter_status.current_term() + 1);
    vote_request.set_candidate_id(18063);
    vote_request.set_candidate_last_log_index(-1);
    vote_request.set_candidate_last_log_term(0);
    kvstore::VoteResponse vote_response;
    grpc::ClientContext vote_context;
    ASSERT_TRUE(voter_stub.RequestVote(&vote_context, vote_request, &vote_response).ok());
    EXPECT_FALSE(vote_response.vote_granted());
}

TEST(RaftIntegration, FollowersReportCurrentLeaderId) {
    const std::vector<std::string> ports = {"18063", "18064", "18065"};
    for (const auto& port : ports) {
        std::error_code error;
        std::filesystem::remove_all(std::filesystem::path("data") / ("node_" + port), error);
    }

    TestCluster cluster(ports);
    for (const auto& port : ports) {
        ASSERT_TRUE(WaitForServer("localhost:" + port));
    }
    ASSERT_TRUE(WaitForLeader("localhost:" + ports.front()));

    std::string leader_port;
    std::vector<std::string> follower_ports;
    for (const auto& port : ports) {
        auto channel = grpc::CreateChannel("localhost:" + port, grpc::InsecureChannelCredentials());
        kvstore::KVStore::Stub stub(channel);
        kvstore::ClusterStatusRequest request;
        kvstore::ClusterStatusResponse response;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        ASSERT_TRUE(stub.GetClusterStatus(&context, request, &response).ok());
        if (response.is_leader()) {
            leader_port = port;
        } else {
            follower_ports.push_back(port);
        }
    }
    ASSERT_FALSE(leader_port.empty());
    ASSERT_FALSE(follower_ports.empty());

    for (const auto& port : follower_ports) {
        auto channel = grpc::CreateChannel("localhost:" + port, grpc::InsecureChannelCredentials());
        kvstore::KVStore::Stub stub(channel);
        kvstore::ClusterStatusRequest request;
        kvstore::ClusterStatusResponse response;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        ASSERT_TRUE(stub.GetClusterStatus(&context, request, &response).ok());
        EXPECT_EQ(response.leader_id(), std::stoi(leader_port));
        EXPECT_FALSE(response.is_leader());
    }
}

TEST(RaftIntegration, LeaderStepsDownWhenItLosesQuorum) {
    const std::vector<std::string> ports = {"18064", "18065", "18066"};
    for (const auto& port : ports) {
        std::error_code error;
        std::filesystem::remove_all(std::filesystem::path("data") / ("node_" + port), error);
    }

    std::vector<pid_t> pids;
    for (const auto& port : ports) {
        std::vector<std::string> peers;
        for (const auto& peer_port : ports) {
            if (peer_port != port) {
                peers.push_back("localhost:" + peer_port);
            }
        }
        pids.push_back(StartServer(port, peers));
    }

    for (const auto& port : ports) {
        ASSERT_TRUE(WaitForServer("localhost:" + port));
    }

    std::string leader_address;
    for (const auto& port : ports) {
        auto channel = grpc::CreateChannel("localhost:" + port, grpc::InsecureChannelCredentials());
        kvstore::KVStore::Stub stub(channel);
        kvstore::ClusterStatusRequest request;
        kvstore::ClusterStatusResponse response;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
        if (stub.GetClusterStatus(&context, request, &response).ok() && response.is_leader()) {
            leader_address = "localhost:" + port;
            break;
        }
    }
    ASSERT_FALSE(leader_address.empty());

    std::string leader_port = leader_address.substr(leader_address.find(':') + 1);
    std::vector<std::string> followers;
    for (const auto& port : ports) {
        if (port != leader_port) {
            followers.push_back(port);
        }
    }
    ASSERT_EQ(followers.size(), 2);

    for (const auto& follower_port : followers) {
        const auto follower_pid = std::stoi(follower_port) == 18064 ? pids[0] :
                                 (std::stoi(follower_port) == 18065 ? pids[1] : pids[2]);
        kill(follower_pid, SIGTERM);
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        auto channel = grpc::CreateChannel(leader_address, grpc::InsecureChannelCredentials());
        kvstore::KVStore::Stub stub(channel);
        kvstore::ClusterStatusRequest request;
        kvstore::ClusterStatusResponse response;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
        if (stub.GetClusterStatus(&context, request, &response).ok() && !response.is_leader()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    auto channel = grpc::CreateChannel(leader_address, grpc::InsecureChannelCredentials());
    kvstore::KVStore::Stub stub(channel);
    kvstore::ClusterStatusRequest request;
    kvstore::ClusterStatusResponse response;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    ASSERT_TRUE(stub.GetClusterStatus(&context, request, &response).ok());
    EXPECT_FALSE(response.is_leader());

    for (const auto pid : pids) {
        if (pid > 0) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
        }
    }
}

TEST(RaftIntegration, EmptyAppendEntriesPreservesMatchingSuffix) {
    const std::string port = "18059";
    const std::string address = "localhost:" + port;
    TestServer server(port);
    ASSERT_GT(server.pid, 0);
    ASSERT_TRUE(WaitForServer(address));
    ASSERT_TRUE(WaitForLeader(address));

    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    kvstore::KVStore::Stub stub(channel);
    for (const auto& key : {"heartbeat-prefix", "heartbeat-suffix"}) {
        kvstore::SetRequest set_request;
        set_request.set_key(key);
        set_request.set_value("value");
        kvstore::SetResponse set_response;
        grpc::ClientContext set_context;
        ASSERT_TRUE(stub.Set(&set_context, set_request, &set_response).ok());
        ASSERT_TRUE(set_response.success());
    }

    kvstore::ClusterStatusRequest cluster_request;
    kvstore::ClusterStatusResponse cluster_response;
    grpc::ClientContext cluster_context;
    ASSERT_TRUE(stub.GetClusterStatus(&cluster_context, cluster_request, &cluster_response).ok());

    kvstore::AppendEntriesRequest append_request;
    append_request.set_leader_term(cluster_response.current_term());
    append_request.set_leader_id(std::stoi(port));
    append_request.set_prev_log_index(cluster_response.commit_index());
    append_request.set_prev_log_term(cluster_response.current_term());
    kvstore::AppendEntriesResponse append_response;
    grpc::ClientContext append_context;
    ASSERT_TRUE(stub.AppendEntries(&append_context, append_request, &append_response).ok());
    ASSERT_TRUE(append_response.success());
    EXPECT_EQ(append_response.match_index(), cluster_response.commit_index());
}

TEST(RaftIntegration, RejectsAppendEntriesWithFutureEntryTerm) {
    const std::string port = "18071";
    const std::string address = "localhost:" + port;
    TestServer server(port);
    ASSERT_GT(server.pid, 0);
    ASSERT_TRUE(WaitForServer(address));
    ASSERT_TRUE(WaitForLeader(address));

    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    kvstore::KVStore::Stub stub(channel);
    kvstore::ClusterStatusRequest status_request;
    kvstore::ClusterStatusResponse status_response;
    grpc::ClientContext status_context;
    ASSERT_TRUE(stub.GetClusterStatus(&status_context, status_request, &status_response).ok());

    kvstore::AppendEntriesRequest append_request;
    append_request.set_leader_term(status_response.current_term());
    append_request.set_leader_id(std::stoi(port));
    append_request.set_prev_log_index(status_response.commit_index());
    append_request.set_prev_log_term(status_response.current_term());
    append_request.add_entries()->set_term(status_response.current_term() + 1);
    kvstore::AppendEntriesResponse append_response;
    grpc::ClientContext append_context;
    const auto status = stub.AppendEntries(&append_context, append_request, &append_response);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(RaftIntegration, RejectsSnapshotWithNegativeIncludedIndex) {
    const std::string port = "18072";
    const std::string address = "localhost:" + port;
    TestServer server(port);
    ASSERT_GT(server.pid, 0);
    ASSERT_TRUE(WaitForServer(address));
    ASSERT_TRUE(WaitForLeader(address));

    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    kvstore::KVStore::Stub stub(channel);
    kvstore::ClusterStatusRequest status_request;
    kvstore::ClusterStatusResponse status_response;
    grpc::ClientContext status_context;
    ASSERT_TRUE(stub.GetClusterStatus(&status_context, status_request, &status_response).ok());

    kvstore::InstallSnapshotRequest snapshot_request;
    snapshot_request.set_leader_term(status_response.current_term());
    snapshot_request.set_leader_id(std::stoi(port));
    snapshot_request.set_last_included_index(-1);
    snapshot_request.set_last_included_term(0);
    snapshot_request.set_done(true);
    kvstore::InstallSnapshotResponse snapshot_response;
    grpc::ClientContext snapshot_context;
    const auto status = stub.InstallSnapshot(&snapshot_context, snapshot_request, &snapshot_response);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

uint32_t ComputeCrc32ForTest(const std::string &payload) {
    constexpr std::uint32_t polynomial = 0xEDB88320u;
    std::uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char byte : payload) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ polynomial;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

std::string SerializeSnapshotPayloadForTest(const std::vector<std::pair<std::string, std::string>>& entries) {
    std::string payload;
    const std::uint32_t count = static_cast<std::uint32_t>(entries.size());
    payload.append(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const auto& [key, value] : entries) {
        const std::uint32_t key_size = static_cast<std::uint32_t>(key.size());
        const std::uint32_t value_size = static_cast<std::uint32_t>(value.size());
        payload.append(reinterpret_cast<const char*>(&key_size), sizeof(key_size));
        payload.append(reinterpret_cast<const char*>(&value_size), sizeof(value_size));
        payload.append(key);
        payload.append(value);
    }
    return payload;
}

TEST(RaftIntegration, WALRecordsIncludeChecksumAndVerifyIntegrity) {
    const std::string port = "18058";
    const std::string address = "localhost:" + port;
    const auto data_path = std::filesystem::path("data") / ("node_" + port);

    {
        TestServer server(port);
        ASSERT_GT(server.pid, 0);
        ASSERT_TRUE(WaitForServer(address));
        ASSERT_TRUE(WaitForLeader(address));

        auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
        kvstore::KVStore::Stub stub(channel);
        kvstore::SetRequest request;
        request.set_key("wal-corruption-check");
        request.set_value("must-survive");
        kvstore::SetResponse response;
        grpc::ClientContext context;
        ASSERT_TRUE(stub.Set(&context, request, &response).ok());
        ASSERT_TRUE(response.success());
    }

    ASSERT_TRUE(std::filesystem::exists(data_path / "raft.wal"));
    std::ifstream input(data_path / "raft.wal", std::ios::binary);
    ASSERT_TRUE(input.is_open());

    std::uint32_t size = 0;
    std::uint32_t checksum = 0;
    ASSERT_TRUE(input.read(reinterpret_cast<char *>(&size), sizeof(size)));
    ASSERT_TRUE(input.read(reinterpret_cast<char *>(&checksum), sizeof(checksum)));
    ASSERT_GT(size, 0u);

    std::string payload(size, '\0');
    ASSERT_TRUE(input.read(payload.data(), static_cast<std::streamsize>(size)));
    EXPECT_EQ(checksum, ComputeCrc32ForTest(payload));
}

TEST(RaftIntegration, FollowersInstallSnapshotWhenBehind) {
    const std::string port = "18057";
    const std::string address = "localhost:" + port;

    TestServer server(port);
    ASSERT_GT(server.pid, 0);
    ASSERT_TRUE(WaitForServer(address));
    ASSERT_TRUE(WaitForLeader(address));

    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    auto stub = kvstore::KVStore::NewStub(channel);
    for (int index = 0; index < 25; ++index) {
        kvstore::SetRequest request;
        request.set_key("snapshot-install-" + std::to_string(index));
        request.set_value("value");
        request.set_request_id("snapshot-install-" + std::to_string(index));
        kvstore::SetResponse response;
        grpc::ClientContext context;
        ASSERT_TRUE(stub->Set(&context, request, &response).ok());
        ASSERT_TRUE(response.success());
    }

    kvstore::InstallSnapshotRequest snapshot_request;
    snapshot_request.set_leader_term(7);
    snapshot_request.set_leader_id(std::stoi(port));
    snapshot_request.set_last_included_index(28);
    snapshot_request.set_last_included_term(7);
    snapshot_request.set_done(true);

    std::vector<std::pair<std::string, std::string>> entries = {
        {"snapshot-install-0", "value"},
        {"snapshot-install-1", "value"},
        {"snapshot-install-2", "value"},
        {"snapshot-install-3", "value"},
    };
    snapshot_request.set_snapshot_data(SerializeSnapshotPayloadForTest(entries));

    kvstore::InstallSnapshotResponse snapshot_response;
    grpc::ClientContext snapshot_context;
    ASSERT_TRUE(stub->InstallSnapshot(&snapshot_context, snapshot_request, &snapshot_response).ok());
    ASSERT_TRUE(snapshot_response.success());
    EXPECT_EQ(snapshot_response.match_index(), 28);

    kvstore::GetRequest get_request;
    get_request.set_key("snapshot-install-0");
    kvstore::GetResponse get_response;
    grpc::ClientContext get_context;
    ASSERT_TRUE(stub->Get(&get_context, get_request, &get_response).ok());
    ASSERT_TRUE(get_response.found());
    EXPECT_EQ(get_response.value(), "value");
}

TEST(RaftIntegration, CompactsCommittedLogIntoSnapshot) {
    const std::string port = "18056";
    const std::string address = "localhost:" + port;
    const auto data_path = std::filesystem::path("data") / ("node_" + port);

    {
        TestServer server(port);
        ASSERT_GT(server.pid, 0);
        ASSERT_TRUE(WaitForServer(address));
        ASSERT_TRUE(WaitForLeader(address));

        auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
        auto stub = kvstore::KVStore::NewStub(channel);
        for (int index = 0; index < 101; ++index) {
            kvstore::SetRequest request;
            request.set_key("snapshot-" + std::to_string(index));
            request.set_value("value");
            kvstore::SetResponse response;
            grpc::ClientContext context;
            ASSERT_TRUE(stub->Set(&context, request, &response).ok());
            ASSERT_TRUE(response.success());
        }
    }

    EXPECT_TRUE(std::filesystem::exists(data_path / "raft.snapshot"));
}

TEST(RaftIntegration, RejectsCorruptSnapshotOnStartup) {
    const std::string port = "18073";
    const auto data_path = std::filesystem::path("data") / ("node_" + port);
    std::error_code error;
    std::filesystem::remove_all(data_path, error);
    std::filesystem::create_directories(data_path);
    {
        std::ofstream snapshot(data_path / "raft.snapshot", std::ios::binary);
        snapshot.write("RKVSNP01", 8);
        const int index = 12;
        const int term = 3;
        const std::uint32_t payload_size = 1;
        const std::uint32_t checksum = 0;
        snapshot.write(reinterpret_cast<const char*>(&index), sizeof(index));
        snapshot.write(reinterpret_cast<const char*>(&term), sizeof(term));
        snapshot.write(reinterpret_cast<const char*>(&payload_size), sizeof(payload_size));
        snapshot.write(reinterpret_cast<const char*>(&checksum), sizeof(checksum));
        snapshot.put('x');
    }

    const pid_t pid = StartServer(port, {});
    ASSERT_GT(pid, 0);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    int status = 0;
    EXPECT_GT(waitpid(pid, &status, WNOHANG), 0);
}

TEST(RaftIntegration, RejectsInvalidMetadataOnStartup) {
    const std::string port = "18074";
    const auto data_path = std::filesystem::path("data") / ("node_" + port);
    std::error_code error;
    std::filesystem::remove_all(data_path, error);
    std::filesystem::create_directories(data_path);
    {
        std::ofstream metadata(data_path / "raft.meta", std::ios::trunc);
        metadata << "-1 0\n";
    }

    const pid_t pid = StartServer(port, {});
    ASSERT_GT(pid, 0);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    int status = 0;
    EXPECT_GT(waitpid(pid, &status, WNOHANG), 0);
}

TEST(RaftIntegration, TruncatedWALTailIsTruncatedAndValidPrefixSurvives) {
    const std::string port = "18075";
    const std::string address = "localhost:" + port;
    const auto data_path = std::filesystem::path("data") / ("node_" + port);

    {
        TestServer server(port);
        ASSERT_GT(server.pid, 0);
        ASSERT_TRUE(WaitForServer(address));
        ASSERT_TRUE(WaitForLeader(address));

        auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
        kvstore::KVStore::Stub stub(channel);
        kvstore::SetRequest request;
        request.set_key("truncated-wal-prefix");
        request.set_value("must-survive");
        request.set_request_id("truncated-wal-prefix");
        kvstore::SetResponse response;
        grpc::ClientContext context;
        ASSERT_TRUE(stub.Set(&context, request, &response).ok());
        ASSERT_TRUE(response.success());
    }

    {
        std::ofstream wal(data_path / "raft.wal", std::ios::binary | std::ios::app);
        wal.write("bad-tail", 8);
    }

    TestServer restarted(port, false);
    ASSERT_GT(restarted.pid, 0);
    ASSERT_TRUE(WaitForServer(address));

    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    kvstore::KVStore::Stub stub(channel);
    kvstore::GetRequest request;
    request.set_key("truncated-wal-prefix");
    kvstore::GetResponse response;
    grpc::ClientContext context;
    ASSERT_TRUE(stub.Get(&context, request, &response).ok());
    ASSERT_TRUE(response.found());
    EXPECT_EQ(response.value(), "must-survive");
}

TEST(RaftIntegration, RepeatedRestartPreservesAcknowledgedWrites) {
    const std::string port = "18076";
    const std::string address = "localhost:" + port;

    for (int cycle = 0; cycle < 3; ++cycle) {
        TestServer server(port, cycle == 0);
        ASSERT_GT(server.pid, 0);
        ASSERT_TRUE(WaitForServer(address));
        ASSERT_TRUE(WaitForLeader(address));

        auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
        kvstore::KVStore::Stub stub(channel);
        kvstore::SetRequest request;
        request.set_key("restart-cycle-" + std::to_string(cycle));
        request.set_value("durable");
        request.set_request_id("restart-cycle-" + std::to_string(cycle));
        kvstore::SetResponse response;
        grpc::ClientContext context;
        ASSERT_TRUE(stub.Set(&context, request, &response).ok());
        ASSERT_TRUE(response.success());
    }

    TestServer final_server(port, false);
    ASSERT_GT(final_server.pid, 0);
    ASSERT_TRUE(WaitForServer(address));

    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    kvstore::KVStore::Stub stub(channel);
    for (int cycle = 0; cycle < 3; ++cycle) {
        kvstore::GetRequest request;
        request.set_key("restart-cycle-" + std::to_string(cycle));
        kvstore::GetResponse response;
        grpc::ClientContext context;
        ASSERT_TRUE(stub.Get(&context, request, &response).ok());
        ASSERT_TRUE(response.found());
        EXPECT_EQ(response.value(), "durable");
    }
}

}  // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
