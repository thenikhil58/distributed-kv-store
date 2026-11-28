#include <iostream>
#include <iomanip>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <memory>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <random>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <initializer_list>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <grpc/impl/codegen/grpc_types.h>
#include <grpcpp/grpcpp.h>
#include "kvstore.grpc.pb.h"
#include "runtime_config.h"
#include "file_credentials.h"

using grpc::ClientContext;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using kvstore::AppendEntriesRequest;
using kvstore::AppendEntriesResponse;
using kvstore::ClusterStatusRequest;
using kvstore::ClusterStatusResponse;
using kvstore::DeleteRequest;
using kvstore::DeleteResponse;
using kvstore::GetRequest;
using kvstore::GetResponse;
using kvstore::HeartbeatRequest;
using kvstore::HeartbeatResponse;
using kvstore::KVStore;
using kvstore::ListKeysRequest;
using kvstore::ListKeysResponse;
using kvstore::LogEntry;
using kvstore::SetRequest;
using kvstore::SetResponse;
using kvstore::VoteRequest;
using kvstore::VoteResponse;

namespace {

std::mutex log_mutex;
std::atomic<int> configured_log_level{1};
volatile std::sig_atomic_t termination_signal = 0;
volatile std::sig_atomic_t shutdown_wakeup_fd = -1;

std::string JsonEscape(const std::string &value)
{
    std::ostringstream escaped;
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': escaped << "\\\""; break;
        case '\\': escaped << "\\\\"; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default:
            if (character < 0x20)
                escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(character) << std::dec;
            else
                escaped << static_cast<char>(character);
        }
    }
    return escaped.str();
}

void ConfigureLogLevel()
{
    const char *level = std::getenv("KV_LOG_LEVEL");
    const std::string configured = level ? level : "info";
    if (configured == "debug")
        configured_log_level = 0;
    else if (configured == "info")
        configured_log_level = 1;
    else if (configured == "warn")
        configured_log_level = 2;
    else if (configured == "error")
        configured_log_level = 3;
    else
        throw std::runtime_error("KV_LOG_LEVEL must be debug, info, warn, or error");
}

void LogEvent(const std::string &level, int level_priority, const std::string &event,
              std::initializer_list<std::pair<std::string, std::string>> fields = {})
{
    if (level_priority < configured_log_level.load())
        return;
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cout << "{\"timestamp_unix_ms\":" << now_ms
              << ",\"level\":\"" << level << "\",\"event\":\""
              << JsonEscape(event) << '"';
    for (const auto &[name, value] : fields)
        std::cout << ",\"" << JsonEscape(name) << "\":\"" << JsonEscape(value) << '"';
    std::cout << "}" << std::endl;
}

void HandleTerminationSignal(int signal_number)
{
    const char wakeup = signal_number == SIGHUP ? 'R' : 'S';
    if (wakeup == 'S')
        termination_signal = signal_number;
    const int wakeup_fd = static_cast<int>(shutdown_wakeup_fd);
    if (wakeup_fd >= 0)
    {
        const auto ignored = write(wakeup_fd, &wakeup, sizeof(wakeup));
        (void)ignored;
    }
}

} // namespace

struct TlsConfig
{
    std::string ca_file;
    std::string certificate_file;
    std::string private_key_file;
    std::string auth_file;
    std::string token_file;
    std::string crl_directory;

    bool Enabled() const
    {
        return !ca_file.empty() && !certificate_file.empty() && !private_key_file.empty();
    }

    bool Configured() const
    {
        return !ca_file.empty() || !certificate_file.empty() || !private_key_file.empty();
    }
};

struct TokenPolicy
{
    std::unordered_set<std::string> roles;
    std::vector<std::string> read_prefixes;
    std::vector<std::string> write_prefixes;
};

using TokenRoles = std::unordered_map<std::string, TokenPolicy>;

static bool IsPrivateFile(const std::filesystem::path &path)
{
    if (std::filesystem::status(path).permissions() == std::filesystem::perms::unknown)
        return false;
    const auto permissions = std::filesystem::status(path).permissions();
    return (permissions & (std::filesystem::perms::group_all |
                           std::filesystem::perms::others_all)) == std::filesystem::perms::none;
}

static std::string Sha256Hex(const std::string &value)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(value.data()), value.size(), digest);
    std::ostringstream result;
    for (const auto byte : digest)
        result << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    return result.str();
}

static TokenRoles LoadTokenRoles(const std::filesystem::path &path)
{
    if (!IsPrivateFile(path))
        throw std::runtime_error("authorization file must have mode 0600 or stricter: " + path.string());
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("failed to open authorization file: " + path.string());

    const std::unordered_set<std::string> valid_roles = {"read", "write", "monitor", "node"};
    TokenRoles roles_by_hash;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line))
    {
        ++line_number;
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || line[first] == '#')
            continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos)
            throw std::runtime_error("invalid authorization file at line " + std::to_string(line_number));
        const auto hash = line.substr(0, separator);
        if (hash.size() != SHA256_DIGEST_LENGTH * 2 ||
            hash.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("authorization token hash must be lowercase SHA-256 hex at line " +
                                     std::to_string(line_number));
        TokenPolicy policy;
        std::istringstream section_stream(line.substr(separator + 1));
        std::string role_section;
        if (!std::getline(section_stream, role_section, ';'))
            throw std::runtime_error("missing authorization roles at line " + std::to_string(line_number));
        std::istringstream role_stream(role_section);
        std::string role;
        while (std::getline(role_stream, role, ','))
        {
            const auto role_start = role.find_first_not_of(" \t\r\n");
            const auto role_end = role.find_last_not_of(" \t\r\n");
            if (role_start == std::string::npos)
                continue;
            role = role.substr(role_start, role_end - role_start + 1);
            if (!valid_roles.count(role))
                throw std::runtime_error("unknown authorization role at line " + std::to_string(line_number));
            policy.roles.insert(role);
        }

        bool read_scope_configured = false;
        bool write_scope_configured = false;
        std::string scope_field;
        while (std::getline(section_stream, scope_field, ';'))
        {
            const auto field_start = scope_field.find_first_not_of(" \t\r\n");
            const auto field_end = scope_field.find_last_not_of(" \t\r\n");
            if (field_start == std::string::npos)
                throw std::runtime_error("empty authorization scope at line " +
                                         std::to_string(line_number));
            scope_field = scope_field.substr(field_start, field_end - field_start + 1);
            const auto scope_separator = scope_field.find('=');
            if (scope_separator == std::string::npos)
                throw std::runtime_error("invalid authorization scope at line " +
                                         std::to_string(line_number));
            const std::string scope_name = scope_field.substr(0, scope_separator);
            auto &prefixes = scope_name == "read_prefixes" ? policy.read_prefixes :
                             scope_name == "write_prefixes" ? policy.write_prefixes :
                             policy.read_prefixes;
            bool *configured = scope_name == "read_prefixes" ? &read_scope_configured :
                               scope_name == "write_prefixes" ? &write_scope_configured : nullptr;
            if (!configured || *configured)
                throw std::runtime_error("unknown or duplicate authorization scope at line " +
                                         std::to_string(line_number));
            *configured = true;
            std::istringstream prefix_stream(scope_field.substr(scope_separator + 1));
            std::string prefix;
            while (std::getline(prefix_stream, prefix, ','))
            {
                const auto prefix_start = prefix.find_first_not_of(" \t\r\n");
                const auto prefix_end = prefix.find_last_not_of(" \t\r\n");
                if (prefix_start == std::string::npos)
                    throw std::runtime_error("empty key prefix at line " + std::to_string(line_number));
                prefix = prefix.substr(prefix_start, prefix_end - prefix_start + 1);
                prefixes.push_back(prefix);
            }
            if (prefixes.empty() ||
                (std::find(prefixes.begin(), prefixes.end(), "*") != prefixes.end() && prefixes.size() != 1))
                throw std::runtime_error("invalid wildcard key scope at line " +
                                         std::to_string(line_number));
        }
        if ((read_scope_configured && !policy.roles.count("read")) ||
            (write_scope_configured && !policy.roles.count("write")))
            throw std::runtime_error("key scope requires its matching role at line " +
                                     std::to_string(line_number));
        if (policy.roles.empty() || !roles_by_hash.emplace(hash, std::move(policy)).second)
            throw std::runtime_error("empty or duplicate authorization entry at line " +
                                     std::to_string(line_number));
    }
    if (roles_by_hash.empty())
        throw std::runtime_error("authorization file must define at least one token");
    return roles_by_hash;
}

static std::string ReadSecretFile(const std::filesystem::path &path)
{
    if (!IsPrivateFile(path))
        throw std::runtime_error("token file must have mode 0600 or stricter: " + path.string());
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("failed to open token file: " + path.string());
    std::string token{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r'))
        token.pop_back();
    if (token.size() < 32)
        throw std::runtime_error("token file must contain at least 32 bytes");
    return token;
}

// The three states a Raft node can be in at any point in time.
enum class NodeState
{
    FOLLOWER,
    CANDIDATE,
    LEADER
};

// Implements the KVStore gRPC service, including the underlying Raft,
// consensus protocol (leader election + log replication).
class KVStoreServiceImpl final : public KVStore::Service
{
private:
    static constexpr const char *kDeleteMarker = "__raft_kv_delete__";
    static constexpr std::size_t kMaxKeyBytes = 4096;
    static constexpr std::size_t kMaxValueBytes = 1024 * 1024;
    static constexpr std::size_t kMaxRequestIdBytes = 256;
    // Applied state machine - only reflects committed log entries.
    std::unordered_map<std::string, std::string> store;
    mutable std::mutex store_mutex;

    struct WriteRequestResult {
        bool success = false;
        int leader_id = -1;
        std::string client_id;
        std::uint64_t request_sequence = 0;
    };

    struct SnapshotState {
        std::unordered_map<std::string, std::string> store;
        std::unordered_map<std::string, WriteRequestResult> completed_requests;
        std::unordered_map<std::string, std::uint64_t> client_sequences;
    };

    std::unordered_map<std::string, WriteRequestResult> completed_write_requests;
    std::unordered_map<std::string, std::uint64_t> completed_client_sequences;
    std::mutex request_cache_mutex;
    std::mutex client_write_mutex;

    static std::string WriteIdentity(const std::string &request_id,
                                     const std::string &client_id,
                                     std::uint64_t request_sequence)
    {
        if (!client_id.empty())
            return "session/" + std::to_string(client_id.size()) + ":" + client_id + ":" +
                   std::to_string(request_sequence);
        return request_id;
    }

    bool TryReplayRequest(const std::string &identity, WriteRequestResult *result)
    {
        if (identity.empty())
            return false;
        std::lock_guard<std::mutex> lock(request_cache_mutex);
        const auto it = completed_write_requests.find(identity);
        if (it == completed_write_requests.end())
            return false;
        if (result)
            *result = it->second;
        return true;
    }

    void RecordWriteRequestResult(const LogEntry &entry, bool success, int leader_id)
    {
        const std::string identity = WriteIdentity(entry.request_id(), entry.client_id(), entry.request_sequence());
        if (identity.empty())
            return;
        std::lock_guard<std::mutex> lock(request_cache_mutex);
        completed_write_requests[identity] =
            WriteRequestResult{success, leader_id, entry.client_id(), entry.request_sequence()};
        if (!entry.client_id().empty())
            completed_client_sequences[entry.client_id()] =
                std::max(completed_client_sequences[entry.client_id()], entry.request_sequence());
    }

    bool IsStaleClientSequence(const std::string &client_id, std::uint64_t sequence)
    {
        if (client_id.empty())
            return false;
        std::lock_guard<std::mutex> lock(request_cache_mutex);
        const auto it = completed_client_sequences.find(client_id);
        return it != completed_client_sequences.end() && sequence <= it->second;
    }

    // Outbound connections to every other node in the cluster.
    std::vector<std::shared_ptr<KVStore::Stub>> peer_stubs;
    std::vector<std::string> peer_addresses;
    std::vector<int> peer_node_ids;
    std::mutex peers_mutex;
    std::filesystem::path peers_file;
    std::filesystem::file_time_type peers_file_timestamp{};

    std::vector<std::shared_ptr<KVStore::Stub>> PeerSnapshot()
    {
        std::lock_guard<std::mutex> lock(peers_mutex);
        return peer_stubs;
    }

    bool IsConfiguredPeer(int candidate_id)
    {
        std::lock_guard<std::mutex> lock(peers_mutex);
        return candidate_id != node_id &&
               std::find(peer_node_ids.begin(), peer_node_ids.end(), candidate_id) != peer_node_ids.end();
    }

    std::shared_ptr<KVStore::Stub> FindLeaderStub()
    {
        std::lock_guard<std::mutex> lock(peers_mutex);
        for (size_t i = 0; i < peer_stubs.size(); ++i)
        {
            ClusterStatusRequest status_req;
            ClusterStatusResponse status_resp;
            ClientContext ctx;
            ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(1));
            AddNodeAuth(ctx);
            const auto status = peer_stubs[i]->GetClusterStatus(&ctx, status_req, &status_resp);
            if (status.ok() && status_resp.is_leader())
                return peer_stubs[i];
        }
        return nullptr;
    }

    template <typename Request, typename Response>
    bool ForwardToLeader(const Request &request, Response *response,
                         std::function<grpc::Status(grpc::ClientContext *, const Request &, Response *)> call)
    {
        if (state == NodeState::LEADER)
            return false;

        auto leader_stub = FindLeaderStub();
        if (!leader_stub)
            return false;

        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        const auto status = call(&ctx, request, response);
        if (!status.ok())
            return false;
        return true;
    }

    void ReloadPeersIfChanged()
    {
        if (peers_file.empty() || !std::filesystem::exists(peers_file))
            return;
        const auto timestamp = std::filesystem::last_write_time(peers_file);
        if (timestamp == peers_file_timestamp)
            return;

        std::ifstream input(peers_file);
        std::vector<std::shared_ptr<KVStore::Stub>> updated;
        std::vector<int> updated_ids;
        std::string address;
        while (std::getline(input, address))
        {
            if (address.empty() || address[0] == '#')
                continue;
            const auto separator = address.rfind(':');
            if (separator == std::string::npos || separator + 1 == address.size())
                throw std::runtime_error("invalid peer endpoint in membership file: " + address);
            updated_ids.push_back(std::stoi(address.substr(separator + 1)));
            auto credentials = tls_config.Enabled()
                ? kvstore_credentials::CreateChannelCredentials(
                      tls_config.ca_file, tls_config.certificate_file,
                      tls_config.private_key_file, tls_config.crl_directory)
                : grpc::InsecureChannelCredentials();
            updated.emplace_back(KVStore::NewStub(grpc::CreateChannel(address, credentials)));
        }
        if (updated.empty())
            throw std::runtime_error("membership file must contain at least one peer");
        {
            std::lock_guard<std::mutex> lock(peers_mutex);
            peer_stubs = std::move(updated);
            peer_node_ids = std::move(updated_ids);
            std::lock_guard<std::mutex> log_lock(log_mutex);
            next_index.assign(peer_stubs.size(), log_offset + (int)log_entries.size());
            match_index.assign(peer_stubs.size(), snapshot_last_index);
        }
        peers_file_timestamp = timestamp;
        LogEvent("info", 1, "peer_endpoints_reloaded",
                 {{"count", std::to_string(peer_stubs.size())}, {"source", peers_file.string()}});
    }

    // Core Raft state.
    std::atomic<NodeState> state{NodeState::FOLLOWER};
    std::atomic<bool> leader_ready{false};
    std::atomic<int> current_term{0};
    int voted_for = -1;
    int known_leader_id = -1;
    int node_id;
    mutable std::mutex election_mutex;

    // Tracks the last time we heard from a leader (used for election timeouts).
    std::chrono::steady_clock::time_point last_heartbeat;
    std::mutex heartbeat_mutex;
    std::atomic<bool> running{true};
    std::condition_variable lifecycle_cv;
    std::mutex lifecycle_mutex;
    std::thread election_thread;
    std::thread quorum_thread;

    // Replicated log and commit tracking.
    std::vector<LogEntry> log_entries;
    int log_offset = 0; // absolute index represented by log_entries[0]
    int snapshot_last_index = -1;
    int snapshot_last_term = 0;
    bool snapshot_has_versioned_format = false;
    int commit_index = -1; // highest log index known to be committed
    int last_applied = -1; // highest log index applied to the state machine
    mutable std::mutex log_mutex;
    std::mutex replication_mutex;
    std::vector<int> next_index;  // leader-only: next log index to send to each peer
    std::vector<int> match_index; // leader-only: highest log index known to be replicated on each peer
    std::filesystem::path data_directory;
    std::ofstream wal;
    TlsConfig tls_config;
    mutable std::mutex credentials_mutex;
    TokenRoles token_roles;
    std::string node_token;
    struct RpcMetric
    {
        std::uint64_t requests = 0;
        std::uint64_t errors = 0;
        std::uint64_t duration_microseconds = 0;
    };
    mutable std::mutex metrics_mutex;
    mutable std::unordered_map<std::string, RpcMetric> rpc_metrics;
    std::atomic<std::uint64_t> wal_records_written{0};
    std::atomic<std::uint64_t> wal_bytes_written{0};
    std::atomic<std::uint64_t> snapshots_written{0};
    std::atomic<std::uint64_t> snapshot_bytes_written{0};

    void RecordRpc(const std::string &method, std::uint64_t duration_microseconds)
    {
        std::lock_guard<std::mutex> lock(metrics_mutex);
        auto &metric = rpc_metrics[method];
        ++metric.requests;
        metric.duration_microseconds += duration_microseconds;
    }

    void RecordRpcError(const std::string &method) const
    {
        std::lock_guard<std::mutex> lock(metrics_mutex);
        ++rpc_metrics[method].errors;
    }

    Status RpcError(const std::string &method, grpc::StatusCode code,
                    const std::string &message) const
    {
        RecordRpcError(method);
        return Status(code, message);
    }

    class RpcMetricTimer
    {
    public:
        RpcMetricTimer(KVStoreServiceImpl &owner, std::string method)
            : owner_(owner), method_(std::move(method)), started_(std::chrono::steady_clock::now())
        {
        }

        ~RpcMetricTimer()
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started_).count();
            owner_.RecordRpc(method_, static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed)));
        }

    private:
        KVStoreServiceImpl &owner_;
        std::string method_;
        std::chrono::steady_clock::time_point started_;
    };

    void AddNodeAuth(ClientContext &context) const
    {
        std::string token;
        {
            std::lock_guard<std::mutex> lock(credentials_mutex);
            token = node_token;
        }
        if (!token.empty())
            context.AddMetadata("authorization", "Bearer " + token);
    }

    static void AddForwardedAuth(const ServerContext *incoming, ClientContext &outgoing)
    {
        const auto token = incoming->client_metadata().find("authorization");
        if (token != incoming->client_metadata().end())
            outgoing.AddMetadata("authorization", std::string(token->second.data(), token->second.size()));
    }

    static bool PolicyAllowsKey(const TokenPolicy &policy, const std::string &role,
                                const std::string &key)
    {
        const auto &prefixes = role == "read" ? policy.read_prefixes : policy.write_prefixes;
        if (prefixes.empty())
            return true;
        return std::any_of(prefixes.begin(), prefixes.end(), [&](const std::string &prefix) {
            return prefix == "*" || key.compare(0, prefix.size(), prefix) == 0;
        });
    }

    Status Authorize(const ServerContext *context, const std::string &required_role,
                     const std::string &method,
                     const std::string *key = nullptr,
                     TokenPolicy *authorized_policy = nullptr) const
    {
        if (tls_config.auth_file.empty())
            return Status::OK;
        auto reject = [this, &method](grpc::StatusCode code, const char *message) {
            RecordRpcError(method);
            return Status(code, message);
        };
        const auto token_metadata = context->client_metadata().equal_range("authorization");
        if (token_metadata.first == token_metadata.second)
            return reject(grpc::StatusCode::UNAUTHENTICATED, "authorization token is required");

        if (std::next(token_metadata.first) != token_metadata.second)
            return reject(grpc::StatusCode::UNAUTHENTICATED, "multiple authorization tokens are not allowed");
        const std::string value(token_metadata.first->second.data(), token_metadata.first->second.size());
        constexpr const char *prefix = "Bearer ";
        if (value.compare(0, 7, prefix) != 0 || value.size() <= 7)
            return reject(grpc::StatusCode::UNAUTHENTICATED, "invalid authorization token");
        std::lock_guard<std::mutex> lock(credentials_mutex);
        const auto identity = token_roles.find(Sha256Hex(value.substr(7)));
        if (identity == token_roles.end())
            return reject(grpc::StatusCode::UNAUTHENTICATED, "invalid authorization token");
        if (!identity->second.roles.count(required_role))
            return reject(grpc::StatusCode::PERMISSION_DENIED, "authorization role is insufficient");
        if (key && !PolicyAllowsKey(identity->second, required_role, *key))
            return reject(grpc::StatusCode::PERMISSION_DENIED, "authorization key scope is insufficient");
        if (authorized_policy)
            *authorized_policy = identity->second;
        return Status::OK;
    }

    static std::uint32_t ComputeCrc32(const std::string &payload)
    {
        constexpr std::uint32_t polynomial = 0xEDB88320u;
        std::uint32_t crc = 0xFFFFFFFFu;
        for (const unsigned char byte : payload)
        {
            crc ^= byte;
            for (int bit = 0; bit < 8; ++bit)
            {
                if (crc & 1u)
                    crc = (crc >> 1) ^ polynomial;
                else
                    crc >>= 1;
            }
        }
        return crc ^ 0xFFFFFFFFu;
    }

    static void SyncFilePath(const std::filesystem::path &path)
    {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0)
            throw std::system_error(errno, std::generic_category(), "failed to open for fsync: " + path.string());

        const int sync_result = ::fsync(fd);
        const int sync_error = errno;
        const int close_result = ::close(fd);
        if (sync_result != 0)
            throw std::system_error(sync_error, std::generic_category(), "failed to fsync: " + path.string());
        if (close_result != 0)
            throw std::system_error(errno, std::generic_category(), "failed to close: " + path.string());
    }

    static void SyncDirectoryPath(const std::filesystem::path &path)
    {
        const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd < 0)
            throw std::system_error(errno, std::generic_category(), "failed to open directory for fsync: " + path.string());
        const int sync_result = ::fsync(fd);
        const int sync_error = errno;
        const int close_result = ::close(fd);
        if (sync_result != 0)
            throw std::system_error(sync_error, std::generic_category(), "failed to fsync directory: " + path.string());
        if (close_result != 0)
            throw std::system_error(errno, std::generic_category(), "failed to close directory: " + path.string());
    }

    static bool ReadRecord(std::ifstream &input, std::string &record)
    {
        std::uint32_t size = 0;
        std::uint32_t checksum = 0;
        input.read(reinterpret_cast<char *>(&size), sizeof(size));
        if (!input)
            return false;
        if (size > 64 * 1024 * 1024)
            throw std::runtime_error("WAL record is unreasonably large");
        input.read(reinterpret_cast<char *>(&checksum), sizeof(checksum));
        if (!input)
            throw std::runtime_error("WAL contains a truncated checksum");
        record.resize(size);
        input.read(record.data(), static_cast<std::streamsize>(size));
        if (!input)
            throw std::runtime_error("WAL contains a truncated record");
        if (ComputeCrc32(record) != checksum)
            return false;
        return true;
    }

    static SnapshotState ParseSnapshotPayload(const std::string &payload)
    {
        SnapshotState snapshot;
        if (payload.size() > 256 * 1024 * 1024)
            throw std::runtime_error("snapshot payload exceeds the supported size limit");
        constexpr char magic[] = "RKVSNAP1";
        if (payload.size() >= sizeof(magic) - 1 &&
            payload.compare(0, sizeof(magic) - 1, magic, sizeof(magic) - 1) == 0)
        {
            kvstore::SnapshotData data;
            if (!data.ParseFromArray(payload.data() + sizeof(magic) - 1,
                                     static_cast<int>(payload.size() - (sizeof(magic) - 1))))
                throw std::runtime_error("snapshot protobuf payload is invalid");
            for (const auto &entry : data.entries())
                snapshot.store[entry.key()] = entry.value();
            for (const auto &result : data.completed_requests())
            {
                const std::string identity = WriteIdentity(
                    result.request_id(), result.client_id(), result.request_sequence());
                snapshot.completed_requests[identity] = WriteRequestResult{
                    result.success(), result.leader_id(), result.client_id(), result.request_sequence()};
            }
            for (const auto &[client_id, sequence] : data.client_sequences())
                snapshot.client_sequences[client_id] = sequence;
            return snapshot;
        }
        if (payload.empty())
            return snapshot;

        std::size_t offset = 0;
        std::uint32_t count = 0;
        if (payload.size() < sizeof(count))
            throw std::runtime_error("snapshot payload is truncated");
        std::memcpy(&count, payload.data() + offset, sizeof(count));
        offset += sizeof(count);

        for (std::uint32_t i = 0; i < count; ++i)
        {
            if (offset + sizeof(std::uint32_t) * 2 > payload.size())
                throw std::runtime_error("snapshot payload has truncated key/value sizes");
            std::uint32_t key_size = 0;
            std::uint32_t value_size = 0;
            std::memcpy(&key_size, payload.data() + offset, sizeof(key_size));
            offset += sizeof(key_size);
            std::memcpy(&value_size, payload.data() + offset, sizeof(value_size));
            offset += sizeof(value_size);

            if (key_size > 64 * 1024 * 1024 || value_size > 64 * 1024 * 1024)
                throw std::runtime_error("snapshot payload contains an invalid record size");
            if (offset + key_size + value_size > payload.size())
                throw std::runtime_error("snapshot payload is truncated");

            std::string key(payload.data() + offset, key_size);
            offset += key_size;
            std::string value(payload.data() + offset, value_size);
            offset += value_size;
            snapshot.store.emplace(std::move(key), std::move(value));
        }
        return snapshot;
    }

    static std::string SerializeSnapshotPayload(
        const std::unordered_map<std::string, std::string> &snapshot_store,
        const std::unordered_map<std::string, WriteRequestResult> &completed_requests,
        const std::unordered_map<std::string, std::uint64_t> &client_sequences)
    {
        kvstore::SnapshotData data;
        for (const auto &[key, value] : snapshot_store)
        {
            auto *entry = data.add_entries();
            entry->set_key(key);
            entry->set_value(value);
        }
        for (const auto &[identity, result] : completed_requests)
        {
            auto *saved_result = data.add_completed_requests();
            if (result.client_id.empty())
                saved_result->set_request_id(identity);
            else
                saved_result->set_client_id(result.client_id);
            saved_result->set_request_sequence(result.request_sequence);
            saved_result->set_success(result.success);
            saved_result->set_leader_id(result.leader_id);
        }
        for (const auto &[client_id, sequence] : client_sequences)
            (*data.mutable_client_sequences())[client_id] = sequence;
        std::string serialized;
        if (!data.SerializeToString(&serialized))
            throw std::runtime_error("failed to serialize snapshot data");
        return std::string("RKVSNAP1") + serialized;
    }

    std::string SerializeSnapshotPayload()
    {
        std::unordered_map<std::string, std::string> snapshot_store;
        std::unordered_map<std::string, WriteRequestResult> completed_requests;
        std::unordered_map<std::string, std::uint64_t> client_sequences;
        {
            std::lock_guard<std::mutex> lock(store_mutex);
            snapshot_store = store;
        }
        {
            std::lock_guard<std::mutex> lock(request_cache_mutex);
            completed_requests = completed_write_requests;
            client_sequences = completed_client_sequences;
        }
        return SerializeSnapshotPayload(snapshot_store, completed_requests, client_sequences);
    }

    static void WriteRecord(std::ofstream &output, const std::filesystem::path &path, const std::string &payload)
    {
        const auto size = static_cast<std::uint32_t>(payload.size());
        const auto checksum = ComputeCrc32(payload);
        output.clear();
        output.write(reinterpret_cast<const char *>(&size), sizeof(size));
        output.write(reinterpret_cast<const char *>(&checksum), sizeof(checksum));
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        output.flush();
        if (!output)
            throw std::runtime_error("failed to flush WAL");
        SyncFilePath(path);
    }

    void PersistRecord(const LogEntry &entry)
    {
        LogEntry indexed_entry = entry;
        if (indexed_entry.log_index_plus_one() == 0)
        {
            const int index = log_offset + static_cast<int>(log_entries.size()) - 1;
            indexed_entry.set_log_index_plus_one(static_cast<std::uint64_t>(index) + 1);
        }
        std::string serialized;
        if (!indexed_entry.SerializeToString(&serialized))
            throw std::runtime_error("failed to serialize WAL record");
        wal.clear();
        WriteRecord(wal, data_directory / "raft.wal", serialized);
        wal_records_written.fetch_add(1);
        wal_bytes_written.fetch_add(serialized.size() + sizeof(std::uint32_t) * 2);
    }

    void RewriteWal()
    {
        const auto wal_path = data_directory / "raft.wal";
        const auto temporary_path = data_directory / "raft.wal.tmp";
        wal.close();
        wal.clear();
        try
        {
            std::uint64_t rewritten_records = 0;
            std::uint64_t rewritten_bytes = 0;
            {
                std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
                if (!output)
                    throw std::runtime_error("failed to create temporary WAL: " + temporary_path.string());
                for (std::size_t i = 0; i < log_entries.size(); ++i)
                {
                    LogEntry indexed_entry = log_entries[i];
                    indexed_entry.set_log_index_plus_one(static_cast<std::uint64_t>(log_offset + static_cast<int>(i)) + 1);
                    std::string serialized;
                    if (!indexed_entry.SerializeToString(&serialized))
                        throw std::runtime_error("failed to serialize WAL record");
                    const auto size = static_cast<std::uint32_t>(serialized.size());
                    const auto checksum = ComputeCrc32(serialized);
                    output.write(reinterpret_cast<const char *>(&size), sizeof(size));
                    output.write(reinterpret_cast<const char *>(&checksum), sizeof(checksum));
                    output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
                    ++rewritten_records;
                    rewritten_bytes += serialized.size() + sizeof(size) + sizeof(checksum);
                }
                output.flush();
                if (!output)
                    throw std::runtime_error("failed to rewrite WAL");
                SyncFilePath(temporary_path);
            }
            std::filesystem::rename(temporary_path, wal_path);
            SyncDirectoryPath(data_directory);
            wal = std::ofstream(wal_path, std::ios::binary | std::ios::app);
            if (!wal)
                throw std::runtime_error("failed to reopen WAL");
            wal_records_written.fetch_add(rewritten_records);
            wal_bytes_written.fetch_add(rewritten_bytes);
        }
        catch (...)
        {
            if (!wal.is_open())
            {
                wal.clear();
                wal.open(wal_path, std::ios::binary | std::ios::app);
            }
            throw;
        }
    }

    void LoadPersistentState()
    {
        std::filesystem::create_directories(data_directory);
        LoadSnapshot();
        const auto metadata_path = data_directory / "raft.meta";
        {
            std::ifstream metadata(metadata_path);
            if (metadata)
            {
                int recovered_term = 0;
                if (!(metadata >> recovered_term >> voted_for) || recovered_term < 0 ||
                    (metadata >> std::ws && !metadata.eof()))
                    throw std::runtime_error("invalid Raft metadata file: " + metadata_path.string());
                current_term = recovered_term;
            }
        }

        const auto wal_path = data_directory / "raft.wal";
        std::ifstream input(wal_path, std::ios::binary);
        std::string record;
        bool truncated_tail = false;
        int wal_index = snapshot_has_versioned_format ? 0 : snapshot_last_index + 1;
        while (input)
        {
            try
            {
                if (!ReadRecord(input, record))
                {
                    truncated_tail = true;
                    break;
                }
            }
            catch (const std::runtime_error &error)
            {
                truncated_tail = true;
                LogEvent("warn", 2, "wal_trailing_record_ignored", {{"error", error.what()}});
                break;
            }

            LogEntry entry;
            if (!entry.ParseFromString(record))
            {
                truncated_tail = true;
                LogEvent("warn", 2, "wal_trailing_record_invalid");
                break;
            }
            if (entry.log_index_plus_one() != 0)
            {
                const auto absolute_index = entry.log_index_plus_one() - 1;
                if (absolute_index > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
                    throw std::runtime_error("WAL log index exceeds supported range");
                if (entry.term() < 0)
                    throw std::runtime_error("WAL entry has a negative term");
                if (entry.key().size() > kMaxKeyBytes ||
                    entry.value().size() > kMaxValueBytes ||
                    entry.request_id().size() > kMaxRequestIdBytes ||
                    entry.client_id().size() > kMaxRequestIdBytes)
                    throw std::runtime_error("WAL entry exceeds configured size limits");
                if (absolute_index >= static_cast<std::uint64_t>(snapshot_last_index + 1))
                {
                    if (absolute_index != static_cast<std::uint64_t>(log_offset + static_cast<int>(log_entries.size())))
                        throw std::runtime_error("WAL log indices are not contiguous");
                    log_entries.push_back(std::move(entry));
                }
                wal_index = static_cast<int>(absolute_index + 1);
            }
            else if (wal_index > snapshot_last_index)
            {
                if (entry.term() < 0)
                    throw std::runtime_error("legacy WAL entry has a negative term");
                if (entry.key().size() > kMaxKeyBytes ||
                    entry.value().size() > kMaxValueBytes ||
                    entry.request_id().size() > kMaxRequestIdBytes ||
                    entry.client_id().size() > kMaxRequestIdBytes)
                    throw std::runtime_error("legacy WAL entry exceeds configured size limits");
                entry.set_log_index_plus_one(static_cast<std::uint64_t>(wal_index) + 1);
                log_entries.push_back(std::move(entry));
                ++wal_index;
            }
        }
        if (truncated_tail)
        {
            input.close();
            LogEvent("warn", 2, "wal_corrupt_tail_truncated");
            RewriteWal();
        }
        else
        {
            wal.open(wal_path, std::ios::binary | std::ios::app);
            if (!wal)
                throw std::runtime_error("failed to open WAL: " + wal_path.string());
        }
        LogEvent("info", 1, "raft_wal_recovered",
                 {{"entries", std::to_string(log_entries.size())}, {"path", wal_path.string()}});
    }

    void PersistMetadata()
    {
        const auto temporary = data_directory / "raft.meta.tmp";
        const auto metadata_path = data_directory / "raft.meta";
        {
            std::ofstream metadata(temporary, std::ios::trunc);
            metadata << current_term << " " << voted_for << "\n";
            metadata.flush();
            if (!metadata)
                throw std::runtime_error("failed to write Raft metadata");
            SyncFilePath(temporary);
        }
        std::filesystem::rename(temporary, metadata_path);
        SyncDirectoryPath(data_directory);
    }

    void LoadSnapshot()
    {
        const auto snapshot_path = data_directory / "raft.snapshot";
        std::ifstream input(snapshot_path, std::ios::binary);
        if (!input)
            return;

        const std::string file_data{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        constexpr char magic[] = "RKVSNP01";
        if (file_data.size() >= sizeof(magic) - 1 &&
            file_data.compare(0, sizeof(magic) - 1, magic, sizeof(magic) - 1) == 0)
        {
            snapshot_has_versioned_format = true;
            constexpr std::size_t header_size = sizeof(magic) - 1 + sizeof(snapshot_last_index) +
                                                sizeof(snapshot_last_term) + sizeof(std::uint32_t) * 2;
            if (file_data.size() < header_size)
                throw std::runtime_error("truncated Raft snapshot header");
            std::size_t offset = sizeof(magic) - 1;
            std::uint32_t payload_size = 0;
            std::uint32_t checksum = 0;
            std::memcpy(&snapshot_last_index, file_data.data() + offset, sizeof(snapshot_last_index));
            offset += sizeof(snapshot_last_index);
            std::memcpy(&snapshot_last_term, file_data.data() + offset, sizeof(snapshot_last_term));
            offset += sizeof(snapshot_last_term);
            std::memcpy(&payload_size, file_data.data() + offset, sizeof(payload_size));
            offset += sizeof(payload_size);
            std::memcpy(&checksum, file_data.data() + offset, sizeof(checksum));
            offset += sizeof(checksum);
            if (payload_size > 256 * 1024 * 1024 ||
                file_data.size() != header_size + payload_size)
                throw std::runtime_error("invalid Raft snapshot payload size");
            if (snapshot_last_index < 0 || snapshot_last_term < 0 ||
                snapshot_last_term > snapshot_last_index)
                throw std::runtime_error("invalid Raft snapshot metadata");
            const std::string payload = file_data.substr(offset, payload_size);
            if (ComputeCrc32(payload) != checksum)
                throw std::runtime_error("Raft snapshot checksum mismatch");
            auto snapshot = ParseSnapshotPayload(payload);
            store = std::move(snapshot.store);
            completed_write_requests = std::move(snapshot.completed_requests);
            completed_client_sequences = std::move(snapshot.client_sequences);
            log_offset = snapshot_last_index + 1;
            commit_index = snapshot_last_index;
            last_applied = snapshot_last_index;
            return;
        }

        std::istringstream legacy(file_data, std::ios::binary);
        std::uint32_t count = 0;
        legacy.read(reinterpret_cast<char *>(&snapshot_last_index), sizeof(snapshot_last_index));
        legacy.read(reinterpret_cast<char *>(&snapshot_last_term), sizeof(snapshot_last_term));
        legacy.read(reinterpret_cast<char *>(&count), sizeof(count));
        if (!legacy || count > 10000000)
            throw std::runtime_error("invalid Raft snapshot header");
        if (snapshot_last_index < 0 || snapshot_last_term < 0 ||
            snapshot_last_term > snapshot_last_index)
            throw std::runtime_error("invalid legacy Raft snapshot metadata");

        for (std::uint32_t i = 0; i < count; ++i)
        {
            std::uint32_t key_size = 0;
            std::uint32_t value_size = 0;
            legacy.read(reinterpret_cast<char *>(&key_size), sizeof(key_size));
            legacy.read(reinterpret_cast<char *>(&value_size), sizeof(value_size));
            if (!legacy || key_size > 64 * 1024 * 1024 || value_size > 64 * 1024 * 1024)
                throw std::runtime_error("invalid Raft snapshot record");
            std::string key(key_size, '\0');
            std::string value(value_size, '\0');
            legacy.read(key.data(), key_size);
            legacy.read(value.data(), value_size);
            if (!legacy)
                throw std::runtime_error("truncated Raft snapshot");
            store.emplace(std::move(key), std::move(value));
        }
        if (legacy.peek() != std::char_traits<char>::eof())
            throw std::runtime_error("unexpected trailing bytes in legacy Raft snapshot");
        log_offset = snapshot_last_index + 1;
        commit_index = snapshot_last_index;
        last_applied = snapshot_last_index;
    }

    void PersistSnapshotState(const std::unordered_map<std::string, std::string> &snapshot_store,
                             const std::unordered_map<std::string, WriteRequestResult> &completed_requests,
                             const std::unordered_map<std::string, std::uint64_t> &client_sequences,
                             int last_index, int last_term)
    {
        const auto snapshot_path = data_directory / "raft.snapshot";
        const auto temporary = data_directory / "raft.snapshot.tmp";
        std::uint64_t payload_bytes = 0;
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            const std::string payload = SerializeSnapshotPayload(snapshot_store, completed_requests,
                                                                 client_sequences);
            if (payload.size() > std::numeric_limits<std::uint32_t>::max())
                throw std::runtime_error("Raft snapshot exceeds the supported size limit");
            const auto payload_size = static_cast<std::uint32_t>(payload.size());
            payload_bytes = payload_size;
            const auto checksum = ComputeCrc32(payload);
            output.write("RKVSNP01", 8);
            output.write(reinterpret_cast<const char *>(&last_index), sizeof(last_index));
            output.write(reinterpret_cast<const char *>(&last_term), sizeof(last_term));
            output.write(reinterpret_cast<const char *>(&payload_size), sizeof(payload_size));
            output.write(reinterpret_cast<const char *>(&checksum), sizeof(checksum));
            output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            output.flush();
            if (!output)
                throw std::runtime_error("failed to write Raft snapshot");
            SyncFilePath(temporary);
        }
        std::filesystem::rename(temporary, snapshot_path);
        SyncDirectoryPath(data_directory);
        snapshots_written.fetch_add(1);
        snapshot_bytes_written.fetch_add(payload_bytes);
    }

    void CompactSnapshot()
    {
        if (commit_index - log_offset < 100)
            return;

        const int target = commit_index;
        if (target < log_offset || target >= log_offset + static_cast<int>(log_entries.size()))
            return;
        for (const auto &peer_match : match_index)
        {
            if (peer_match < target)
                return;
        }

        const auto snapshot_term = log_entries[target - log_offset].term();
        std::unordered_map<std::string, WriteRequestResult> completed_requests;
        std::unordered_map<std::string, std::uint64_t> client_sequences;
        {
            std::lock_guard<std::mutex> request_lock(request_cache_mutex);
            completed_requests = completed_write_requests;
            client_sequences = completed_client_sequences;
        }
        try
        {
            PersistSnapshotState(store, completed_requests, client_sequences, target, snapshot_term);
        }
        catch (const std::exception &error)
        {
            LogEvent("error", 3, "snapshot_persist_failed",
                     {{"error", error.what()}, {"action", "retaining_wal"}});
            return;
        }

        log_entries.erase(log_entries.begin(), log_entries.begin() + (target - log_offset + 1));
        log_offset = target + 1;
        snapshot_last_index = target;
        snapshot_last_term = snapshot_term;
        try
        {
            RewriteWal();
        }
        catch (const std::exception &error)
        {
            LogEvent("error", 3, "wal_compaction_failed",
                     {{"error", error.what()}, {"snapshot_remains_usable", "true"}});
        }
        LogEvent("info", 1, "raft_log_compacted", {{"last_index", std::to_string(target)}});
    }

    // Generates a randomized election timeout to reduce the chance of
    // multiple nodes becoming candidates simultaneously (split votes).
    int RandomElectionTimeout()
    {
        static std::mt19937 rng(std::random_device{}());
        static std::uniform_int_distribution<int> dist(3000, 5000);
        return dist(rng);
    }

    // Applies any newly committed log entries to the state machine.
    // Caller must hold log_mutex.
    void ApplyCommitted()
    {
        while (last_applied < commit_index)
        {
            last_applied++;
            const auto &e = log_entries[last_applied - log_offset];
            if (e.no_op())
                continue;
            if (!e.request_id().empty() || !e.client_id().empty())
                RecordWriteRequestResult(e, true, known_leader_id);
            std::lock_guard<std::mutex> store_lock(store_mutex);
            if (e.value() == kDeleteMarker)
                store.erase(e.key());
            else
                store[e.key()] = e.value();
            LogEvent("debug", 0, "raft_entry_applied",
                     {{"index", std::to_string(last_applied)}, {"node_id", std::to_string(node_id)}});
        }
    }

    // Sends AppendEntries to a single peer, backing off and retrying with an
    // earlier prevLogIndex whenever the peer reports a log mismatch. This is
    // how a peer that has fallen behind automatically catches up.
    bool SendInstallSnapshot(size_t peer_idx)
    {
        const auto peers = PeerSnapshot();
        if (peer_idx >= peers.size())
            return false;

        kvstore::InstallSnapshotRequest req;
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            req.set_leader_term(current_term);
            req.set_leader_id(node_id);
            req.set_last_included_index(snapshot_last_index);
            req.set_last_included_term(snapshot_last_term);
            req.set_snapshot_data(SerializeSnapshotPayload());
            req.set_done(true);
        }

        kvstore::InstallSnapshotResponse resp;
        ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        AddNodeAuth(ctx);
        const auto status = peers[peer_idx]->InstallSnapshot(&ctx, req, &resp);
        if (!status.ok())
            return false;
        if (!resp.success())
            return false;

        std::lock_guard<std::mutex> lock(log_mutex);
        next_index[peer_idx] = std::max(next_index[peer_idx], snapshot_last_index + 1);
        match_index[peer_idx] = snapshot_last_index;
        return true;
    }

    bool SendAppendEntries(size_t peer_idx)
    {
        const auto peers = PeerSnapshot();
        if (peer_idx >= peers.size())
            return false;
        while (true)
        {
            int ni;
            bool install_snapshot = false;
            {
                std::lock_guard<std::mutex> lock(log_mutex);
                ni = next_index[peer_idx];
                install_snapshot = ni <= snapshot_last_index;
            }
            if (install_snapshot)
                return SendInstallSnapshot(peer_idx);

            AppendEntriesRequest req;
            {
                std::lock_guard<std::mutex> lock(log_mutex);
                ni = next_index[peer_idx];
                req.set_leader_term(current_term);
                req.set_leader_id(node_id);
                req.set_prev_log_index(ni - 1);
                if (ni - 1 == snapshot_last_index)
                    req.set_prev_log_term(snapshot_last_term);
                else if (ni - 1 >= log_offset)
                    req.set_prev_log_term(log_entries[ni - 1 - log_offset].term());
                else
                    req.set_prev_log_term(0);
                for (int i = ni; i < log_offset + (int)log_entries.size(); i++)
                {
                    *req.add_entries() = log_entries[i - log_offset];
                }
                req.set_leader_commit(commit_index);
            }

            AppendEntriesResponse resp;
            ClientContext ctx;
            ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(500));
            AddNodeAuth(ctx);
            Status status = peers[peer_idx]->AppendEntries(&ctx, req, &resp);

            if (!status.ok())
                return false; // peer unreachable this round

            if (resp.term() > req.leader_term())
            {
                std::lock_guard<std::mutex> election_lock(election_mutex);
                if (resp.term() > current_term)
                {
                    current_term = resp.term();
                    voted_for = -1;
                    PersistMetadata();
                    state = NodeState::FOLLOWER;
                    leader_ready = false;
                }
                return false;
            }

            if (resp.success())
            {
                std::lock_guard<std::mutex> lock(log_mutex);
                match_index[peer_idx] = resp.match_index();
                next_index[peer_idx] = resp.match_index() + 1;
                return true;
            }
            else
            {
                // Log mismatch - step back one index and retry.
                std::lock_guard<std::mutex> lock(log_mutex);
                if (next_index[peer_idx] > 0)
                {
                    next_index[peer_idx]--;
                }
                else
                {
                    return false;
                }
            }
        }
    }

    // Runs a full election cycle: increment term, vote for self, request
    // votes from every peer, and become leader on majority.
    void StartElection()
    {
        int observed_term;
        int candidate_last_log_index;
        int candidate_last_log_term;
        {
            std::lock_guard<std::mutex> lock(election_mutex);
            if (state == NodeState::LEADER)
                return;
            observed_term = current_term;
        }
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            candidate_last_log_index = log_offset + static_cast<int>(log_entries.size()) - 1;
            candidate_last_log_term = log_entries.empty() ? snapshot_last_term : log_entries.back().term();
        }

        const auto peers = PeerSnapshot();
        const int total_nodes = static_cast<int>(peers.size()) + 1;
        const int majority = (total_nodes / 2) + 1;
        const int proposed_term = observed_term + 1;
        int pre_votes = 1;
        for (const auto &stub : peers)
        {
            VoteRequest request;
            request.set_candidate_term(proposed_term);
            request.set_candidate_id(node_id);
            request.set_candidate_last_log_index(candidate_last_log_index);
            request.set_candidate_last_log_term(candidate_last_log_term);
            request.set_pre_vote(true);
            VoteResponse response;
            ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(500));
            AddNodeAuth(context);
            const auto status = stub->RequestVote(&context, request, &response);
            if (status.ok() && response.voter_term() > observed_term)
            {
                std::lock_guard<std::mutex> lock(election_mutex);
                if (response.voter_term() > current_term)
                {
                    current_term = response.voter_term();
                    voted_for = -1;
                    PersistMetadata();
                    state = NodeState::FOLLOWER;
                }
                return;
            }
            if (status.ok() && response.vote_granted())
                ++pre_votes;
        }

        if (pre_votes < majority)
        {
            LogEvent("debug", 0, "raft_pre_vote_failed",
                     {{"votes", std::to_string(pre_votes)}, {"cluster_size", std::to_string(total_nodes)}});
            return;
        }

        int my_term;
        {
            std::lock_guard<std::mutex> lock(election_mutex);
            if (state == NodeState::LEADER || current_term != observed_term)
                return;
            current_term = proposed_term;
            voted_for = node_id;
            PersistMetadata();
            state = NodeState::CANDIDATE;
            my_term = current_term;
            LogEvent("info", 1, "raft_candidate_started",
                     {{"term", std::to_string(current_term)}, {"node_id", std::to_string(node_id)}});
        }

        int votes = 1; // vote for self
        for (const auto &stub : peers)
        {
            VoteRequest req;
            req.set_candidate_term(my_term);
            req.set_candidate_id(node_id);
            req.set_candidate_last_log_index(candidate_last_log_index);
            req.set_candidate_last_log_term(candidate_last_log_term);
            VoteResponse resp;
            ClientContext ctx;
            ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(1000));
            AddNodeAuth(ctx);
            Status status = stub->RequestVote(&ctx, req, &resp);
            if (status.ok() && resp.voter_term() > my_term)
            {
                std::lock_guard<std::mutex> lock(election_mutex);
                if (resp.voter_term() > current_term)
                {
                    current_term = resp.voter_term();
                    voted_for = -1;
                    PersistMetadata();
                }
                state = NodeState::FOLLOWER;
                return;
            }
            if (status.ok() && resp.vote_granted())
                votes++;
        }

        bool won_election = false;
        {
            std::lock_guard<std::mutex> lock(election_mutex);
            if (state != NodeState::CANDIDATE || current_term != my_term)
                return;

            if (votes >= majority)
            {
                state = NodeState::LEADER;
                leader_ready = false;
                known_leader_id = node_id;
                LogEvent("info", 1, "raft_leader_elected",
                         {{"term", std::to_string(current_term)}, {"node_id", std::to_string(node_id)},
                          {"votes", std::to_string(votes)}, {"cluster_size", std::to_string(total_nodes)}});
                won_election = true;
            }
            else
            {
                state = NodeState::FOLLOWER;
                leader_ready = false;
                LogEvent("info", 1, "raft_election_failed",
                         {{"votes", std::to_string(votes)}, {"cluster_size", std::to_string(total_nodes)}});
            }
        }

        if (won_election)
        {
            {
                std::lock_guard<std::mutex> log_lock(log_mutex);
                for (size_t i = 0; i < next_index.size(); i++)
                {
                    next_index[i] = log_offset + static_cast<int>(log_entries.size());
                    match_index[i] = snapshot_last_index;
                }
            }

            LogEntry no_op;
            no_op.set_term(my_term);
            no_op.set_no_op(true);
            if (CommitEntry(no_op))
            {
                std::lock_guard<std::mutex> lock(election_mutex);
                if (state == NodeState::LEADER && current_term == my_term)
                    leader_ready = true;
            }
        }
    }

    bool CommitEntry(const LogEntry &entry)
    {
        std::lock_guard<std::mutex> replication_lock(replication_mutex);
        if (state != NodeState::LEADER)
            return false;

        int new_index = -1;
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            const std::string identity = WriteIdentity(
                entry.request_id(), entry.client_id(), entry.request_sequence());
            if (!identity.empty())
            {
                for (std::size_t i = 0; i < log_entries.size(); ++i)
                {
                    if (WriteIdentity(log_entries[i].request_id(), log_entries[i].client_id(),
                                      log_entries[i].request_sequence()) == identity)
                    {
                        new_index = log_offset + static_cast<int>(i);
                        break;
                    }
                }
            }
            if (new_index < 0)
            {
                log_entries.push_back(entry);
                try
                {
                    PersistRecord(entry);
                }
                catch (const std::exception &error)
                {
                    log_entries.pop_back();
                    LogEvent("error", 3, "raft_wal_append_failed", {{"error", error.what()}});
                    return false;
                }
                new_index = log_offset + static_cast<int>(log_entries.size()) - 1;
            }
        }

        const auto peers = PeerSnapshot();
        for (size_t i = 0; i < peers.size(); i++)
        {
            SendAppendEntries(i);
        }

        bool committed = false;
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            const int majority = (static_cast<int>(peers.size()) + 1) / 2 + 1;
            const int last_index = log_offset + static_cast<int>(log_entries.size()) - 1;
            for (int candidate = last_index; candidate > commit_index; --candidate)
            {
                const int offset = candidate - log_offset;
                if (offset < 0 || log_entries[offset].term() != current_term)
                    continue;

                int replicas = 1;
                for (const int peer_match : match_index)
                {
                    if (peer_match >= candidate)
                        ++replicas;
                }
                if (replicas >= majority)
                {
                    commit_index = candidate;
                    ApplyCommitted();
                    CompactSnapshot();
                    break;
                }
            }
            committed = new_index <= commit_index;
        }
        for (size_t i = 0; i < peers.size(); i++)
            SendAppendEntries(i);
        if (!committed)
            LogEvent("warn", 2, "raft_write_not_committed",
                     {{"node_id", std::to_string(node_id)}});
        return committed;
    }

    bool LeaderHasQuorum()
    {
        const int total_nodes = (int)PeerSnapshot().size() + 1;
        const int majority = total_nodes / 2 + 1;
        int active_nodes = 1;
        int observed_term;
        {
            std::lock_guard<std::mutex> lock(election_mutex);
            observed_term = current_term;
        }
        for (const auto &stub : PeerSnapshot())
        {
            HeartbeatRequest request;
            request.set_leader_port(node_id);
            request.set_leader_term(observed_term);
            HeartbeatResponse response;
            ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(1));
            AddNodeAuth(context);
            const auto status = stub->Heartbeat(&context, request, &response);
            if (status.ok() && response.alive() && response.term() == observed_term)
                active_nodes++;
            else if (status.ok() && response.term() > observed_term)
            {
                std::lock_guard<std::mutex> lock(election_mutex);
                if (response.term() > current_term)
                {
                    current_term = response.term();
                    voted_for = -1;
                    PersistMetadata();
                    state = NodeState::FOLLOWER;
                    leader_ready = false;
                }
                return false;
            }
        }
        return active_nodes >= majority;
    }

    bool ConfirmLinearizableRead()
    {
        int read_term;
        {
            std::lock_guard<std::mutex> lock(election_mutex);
            if (state != NodeState::LEADER || !leader_ready)
                return false;
            read_term = current_term;
        }
        if (!LeaderHasQuorum())
            return false;
        std::lock_guard<std::mutex> lock(election_mutex);
        return state == NodeState::LEADER && leader_ready && current_term == read_term;
    }

public:
    void ReloadSecurityFiles()
    {
        if (tls_config.auth_file.empty())
            return;
        auto updated_roles = LoadTokenRoles(tls_config.auth_file);
        auto updated_node_token = ReadSecretFile(tls_config.token_file);
        {
            std::lock_guard<std::mutex> lock(credentials_mutex);
            token_roles = std::move(updated_roles);
            node_token = std::move(updated_node_token);
        }
        LogEvent("info", 1, "security_credentials_reloaded");
    }

    std::string RenderMetrics() const
    {
        std::ostringstream metrics;
        const auto current_state = state.load();
        int term;
        int leader_id;
        {
            std::lock_guard<std::mutex> lock(election_mutex);
            term = current_term;
            leader_id = current_state == NodeState::LEADER ? node_id : known_leader_id;
        }

        int committed;
        int snapshot_index;
        int log_size;
        std::vector<int> peer_lag;
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            committed = commit_index;
            snapshot_index = snapshot_last_index;
            log_size = static_cast<int>(log_entries.size());
            for (const int peer_match : match_index)
                peer_lag.push_back(std::max(0, commit_index - peer_match));
        }

        std::size_t key_count;
        {
            std::lock_guard<std::mutex> lock(store_mutex);
            key_count = store.size();
        }

        const char *state_name = current_state == NodeState::LEADER ? "leader" :
                                 current_state == NodeState::CANDIDATE ? "candidate" : "follower";
        metrics << "# HELP raft_node_state Current Raft node role as a one-hot gauge.\n"
                << "# TYPE raft_node_state gauge\n";
        for (const char *candidate : {"leader", "candidate", "follower"})
            metrics << "raft_node_state{node_id=\"" << node_id << "\",state=\"" << candidate
                    << "\"} " << (state_name == std::string(candidate) ? 1 : 0) << '\n';
        metrics << "# HELP raft_current_term Current Raft term.\n"
                << "# TYPE raft_current_term gauge\n"
                << "raft_current_term{node_id=\"" << node_id << "\"} " << term << '\n'
                << "# HELP raft_leader_id Known leader ID, or -1 if unknown.\n"
                << "# TYPE raft_leader_id gauge\n"
                << "raft_leader_id{node_id=\"" << node_id << "\"} " << leader_id << '\n'
                << "# HELP raft_commit_index Highest committed log index.\n"
                << "# TYPE raft_commit_index gauge\n"
                << "raft_commit_index{node_id=\"" << node_id << "\"} " << committed << '\n'
                << "# HELP raft_snapshot_index Last included snapshot index.\n"
                << "# TYPE raft_snapshot_index gauge\n"
                << "raft_snapshot_index{node_id=\"" << node_id << "\"} " << snapshot_index << '\n'
                << "# HELP raft_log_entries Current in-memory log entries.\n"
                << "# TYPE raft_log_entries gauge\n"
                << "raft_log_entries{node_id=\"" << node_id << "\"} " << log_size << '\n'
                << "# HELP raft_replication_lag_entries Commit-index lag for each peer slot.\n"
                << "# TYPE raft_replication_lag_entries gauge\n";
        for (std::size_t index = 0; index < peer_lag.size(); ++index)
            metrics << "raft_replication_lag_entries{node_id=\"" << node_id
                    << "\",peer_index=\"" << index << "\"} " << peer_lag[index] << '\n';
        metrics << "# HELP raft_store_keys Current number of stored keys.\n"
                << "# TYPE raft_store_keys gauge\n"
                << "raft_store_keys{node_id=\"" << node_id << "\"} " << key_count << '\n'
                << "# HELP raft_wal_records_written_total WAL records appended or rewritten.\n"
                << "# TYPE raft_wal_records_written_total counter\n"
                << "raft_wal_records_written_total{node_id=\"" << node_id << "\"} "
                << wal_records_written.load() << '\n'
                << "# HELP raft_wal_bytes_written_total WAL payload and framing bytes written.\n"
                << "# TYPE raft_wal_bytes_written_total counter\n"
                << "raft_wal_bytes_written_total{node_id=\"" << node_id << "\"} "
                << wal_bytes_written.load() << '\n'
                << "# HELP raft_snapshots_written_total Snapshot files successfully persisted.\n"
                << "# TYPE raft_snapshots_written_total counter\n"
                << "raft_snapshots_written_total{node_id=\"" << node_id << "\"} "
                << snapshots_written.load() << '\n'
                << "# HELP raft_snapshot_bytes_written_total Snapshot payload bytes persisted.\n"
                << "# TYPE raft_snapshot_bytes_written_total counter\n"
                << "raft_snapshot_bytes_written_total{node_id=\"" << node_id << "\"} "
                << snapshot_bytes_written.load() << '\n';

        std::vector<std::pair<std::string, RpcMetric>> rpc_snapshot;
        {
            std::lock_guard<std::mutex> lock(metrics_mutex);
            rpc_snapshot.reserve(rpc_metrics.size());
            for (const auto &entry : rpc_metrics)
                rpc_snapshot.push_back(entry);
        }
        metrics << "# HELP kvstore_rpc_requests_total RPC calls handled by method.\n"
                << "# TYPE kvstore_rpc_requests_total counter\n";
        for (const auto &[method, metric] : rpc_snapshot)
            metrics << "kvstore_rpc_requests_total{node_id=\"" << node_id
                    << "\",method=\"" << JsonEscape(method) << "\"} " << metric.requests << '\n';
        metrics << "# HELP kvstore_rpc_errors_total RPC calls returning a non-OK gRPC status.\n"
                << "# TYPE kvstore_rpc_errors_total counter\n";
        for (const auto &[method, metric] : rpc_snapshot)
            metrics << "kvstore_rpc_errors_total{node_id=\"" << node_id
                    << "\",method=\"" << JsonEscape(method) << "\"} " << metric.errors << '\n';
        metrics << "# HELP kvstore_rpc_duration_seconds_sum Total RPC handler duration.\n"
                << "# TYPE kvstore_rpc_duration_seconds_sum counter\n";
        for (const auto &[method, metric] : rpc_snapshot)
            metrics << "kvstore_rpc_duration_seconds_sum{node_id=\"" << node_id
                    << "\",method=\"" << JsonEscape(method) << "\"} "
                    << static_cast<double>(metric.duration_microseconds) / 1'000'000.0 << '\n';
        metrics << "# HELP kvstore_rpc_duration_seconds_count Number of timed RPC handlers.\n"
                << "# TYPE kvstore_rpc_duration_seconds_count counter\n";
        for (const auto &[method, metric] : rpc_snapshot)
            metrics << "kvstore_rpc_duration_seconds_count{node_id=\"" << node_id
                    << "\",method=\"" << JsonEscape(method) << "\"} " << metric.requests << '\n';

        std::uintmax_t storage_bytes = 0;
        std::error_code file_error;
        for (const char *name : {"raft.meta", "raft.wal", "raft.snapshot"})
        {
            const auto file_size = std::filesystem::file_size(data_directory / name, file_error);
            if (!file_error)
                storage_bytes += file_size;
            file_error.clear();
        }
        metrics << "# HELP raft_storage_bytes Current size of Raft metadata, WAL, and snapshot files.\n"
                << "# TYPE raft_storage_bytes gauge\n"
                << "raft_storage_bytes{node_id=\"" << node_id << "\"} " << storage_bytes << '\n';

        struct rusage usage {};
        if (getrusage(RUSAGE_SELF, &usage) == 0)
        {
            const auto cpu_microseconds =
                usage.ru_utime.tv_sec * 1'000'000LL + usage.ru_utime.tv_usec +
                usage.ru_stime.tv_sec * 1'000'000LL + usage.ru_stime.tv_usec;
#ifdef __APPLE__
            const auto resident_bytes = usage.ru_maxrss;
#else
            const auto resident_bytes = usage.ru_maxrss * 1024LL;
#endif
            metrics << "# HELP process_cpu_seconds_total User and system CPU time.\n"
                    << "# TYPE process_cpu_seconds_total counter\n"
                    << "process_cpu_seconds_total{node_id=\"" << node_id << "\"} "
                    << static_cast<double>(cpu_microseconds) / 1'000'000.0 << '\n'
                    << "# HELP process_max_resident_memory_bytes Maximum resident set size.\n"
                    << "# TYPE process_max_resident_memory_bytes gauge\n"
                    << "process_max_resident_memory_bytes{node_id=\"" << node_id << "\"} "
                    << resident_bytes << '\n';
        }
        return metrics.str();
    }

    KVStoreServiceImpl(int my_port, const std::vector<std::string> &peer_addresses,
                       const std::vector<int> &peer_port_ids,
                       const std::filesystem::path &storage_path,
                       const TlsConfig &security_config,
                       const std::filesystem::path &membership_path)
    {
        node_id = my_port;
        data_directory = storage_path;
        tls_config = security_config;
        if (!tls_config.auth_file.empty())
        {
            token_roles = LoadTokenRoles(tls_config.auth_file);
            node_token = ReadSecretFile(tls_config.token_file);
        }
        peers_file = membership_path;
        last_heartbeat = std::chrono::steady_clock::now();
        LoadPersistentState();

        for (const auto &addr : peer_addresses)
        {
            auto credentials = tls_config.Enabled()
                ? kvstore_credentials::CreateChannelCredentials(
                      tls_config.ca_file, tls_config.certificate_file,
                      tls_config.private_key_file, tls_config.crl_directory)
                : grpc::InsecureChannelCredentials();
            auto channel = grpc::CreateChannel(addr, credentials);
            peer_stubs.emplace_back(KVStore::NewStub(channel));
        }
        peer_node_ids = peer_port_ids;

        next_index.assign(peer_stubs.size(), log_offset + (int)log_entries.size());
        match_index.assign(peer_stubs.size(), snapshot_last_index);

        // Background thread: watches for election timeouts and triggers
        // an election if we haven't heard from a leader recently enough.
        election_thread = std::thread([this]()
                    {
            while (running) {
                try { ReloadPeersIfChanged(); } catch (const std::exception& error) {
                    LogEvent("error", 3, "peer_endpoint_reload_failed", {{"error", error.what()}});
                }
                int timeout_ms = RandomElectionTimeout();
                {
                    std::unique_lock<std::mutex> wait_lock(lifecycle_mutex);
                    lifecycle_cv.wait_for(wait_lock, std::chrono::milliseconds(timeout_ms),
                                          [this] { return !running.load(); });
                }
                if (!running)
                    break;
                if (state == NodeState::LEADER) continue;
                std::chrono::steady_clock::time_point last_seen;
                {
                    std::lock_guard<std::mutex> lock(heartbeat_mutex);
                    last_seen = last_heartbeat;
                }
                auto elapsed = std::chrono::steady_clock::now() - last_seen;
                if (elapsed > std::chrono::milliseconds(timeout_ms)) StartElection();
            } })
            ;

        // Background thread: while we're leader, periodically send
        // heartbeats to all peers so they don't start their own elections.
        quorum_thread = std::thread([this]()
                    {
            while (running) {
                if (state == NodeState::LEADER) {
                    int reachable = 1;
                    int observed_term;
                    {
                        std::lock_guard<std::mutex> lock(election_mutex);
                        observed_term = current_term;
                    }
                    for (auto& stub : PeerSnapshot()) {
                        HeartbeatRequest req;
                        req.set_leader_port(node_id);
                        req.set_leader_term(observed_term);
                        HeartbeatResponse resp;
                        ClientContext ctx;
                        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(500));
                        AddNodeAuth(ctx);
                        const auto status = stub->Heartbeat(&ctx, req, &resp);
                        if (status.ok() && resp.alive() && resp.term() == observed_term) {
                            reachable++;
                        }
                        if (status.ok() && resp.term() > observed_term) {
                            std::lock_guard<std::mutex> lock(election_mutex);
                            if (resp.term() > current_term) {
                                current_term = resp.term();
                                voted_for = -1;
                                PersistMetadata();
                            }
                            state = NodeState::FOLLOWER;
                            leader_ready = false;
                            break;
                        }
                    }
                    const int total_nodes = (int)PeerSnapshot().size() + 1;
                    const int majority = total_nodes / 2 + 1;
                    if (reachable < majority) {
                        std::lock_guard<std::mutex> lock(election_mutex);
                        if (state == NodeState::LEADER) {
                            state = NodeState::FOLLOWER;
                            leader_ready = false;
                            std::lock_guard<std::mutex> hb_lock(heartbeat_mutex);
                            last_heartbeat = std::chrono::steady_clock::now();
                            LogEvent("warn", 2, "raft_leader_lost_quorum",
                                     {{"term", std::to_string(current_term)},
                                      {"node_id", std::to_string(node_id)}});
                        }
                    }
                }
                std::unique_lock<std::mutex> wait_lock(lifecycle_mutex);
                lifecycle_cv.wait_for(wait_lock, std::chrono::milliseconds(1000),
                                      [this] { return !running.load(); });
            } })
            ;
    }

    ~KVStoreServiceImpl() override
    {
        running = false;
        lifecycle_cv.notify_all();
        if (election_thread.joinable())
            election_thread.join();
        if (quorum_thread.joinable())
            quorum_thread.join();
    }

    // Follower-side handler: receives log entries (or heartbeats) from the leader.
    Status AppendEntries(ServerContext *context, const AppendEntriesRequest *request, AppendEntriesResponse *response) override
    {
        RpcMetricTimer metric(*this, "AppendEntries");
        const auto authorization = Authorize(context, "node", "AppendEntries");
        if (!authorization.ok())
            return authorization;
        if (request->leader_id() != node_id && !IsConfiguredPeer(request->leader_id()))
            return RpcError("AppendEntries", grpc::StatusCode::PERMISSION_DENIED,
                            "AppendEntries sender is not a configured peer");
        {
            std::lock_guard<std::mutex> lock(election_mutex);
            if (request->leader_term() < current_term)
            {
                // Stale leader - reject.
                response->set_term(current_term);
                response->set_success(false);
                return Status::OK;
            }
            if (request->entries_size() > 1024)
                return RpcError("AppendEntries", grpc::StatusCode::RESOURCE_EXHAUSTED,
                                "AppendEntries batch exceeds 1024 entries");
            for (const auto &entry : request->entries())
            {
                if (entry.key().size() > kMaxKeyBytes || entry.value().size() > kMaxValueBytes ||
                    entry.request_id().size() > kMaxRequestIdBytes)
                    return RpcError("AppendEntries", grpc::StatusCode::RESOURCE_EXHAUSTED,
                                    "replicated entry exceeds configured size limits");
                if (entry.term() < 0 || entry.term() > request->leader_term())
                    return RpcError("AppendEntries", grpc::StatusCode::INVALID_ARGUMENT,
                                    "AppendEntries entry term is outside the leader term");
            }
            if (request->leader_term() < 0 || request->prev_log_index() < -1 ||
                request->prev_log_index() == std::numeric_limits<int>::max() ||
                request->prev_log_term() < 0 || request->leader_commit() < -1)
                return RpcError("AppendEntries", grpc::StatusCode::INVALID_ARGUMENT,
                                "AppendEntries contains a negative Raft index");
            if (request->prev_log_index() >= 0 &&
                request->entries_size() >
                    std::numeric_limits<int>::max() - request->prev_log_index())
                return RpcError("AppendEntries", grpc::StatusCode::INVALID_ARGUMENT,
                                "AppendEntries index range overflows");
            if (request->leader_term() > current_term)
            {
                current_term = request->leader_term();
                voted_for = -1;
                try
                {
                    PersistMetadata();
                }
                catch (const std::exception &error)
                {
                    return RpcError("AppendEntries", grpc::StatusCode::INTERNAL, error.what());
                }
            }
            state = NodeState::FOLLOWER;
            leader_ready = false;
            known_leader_id = request->leader_id();
        }
        {
            std::lock_guard<std::mutex> hb_lock(heartbeat_mutex);
            last_heartbeat = std::chrono::steady_clock::now();
        }

        std::lock_guard<std::mutex> log_lock(log_mutex);
        int prev_index = request->prev_log_index();
        int prev_term = request->prev_log_term();

        if (prev_index < snapshot_last_index)
        {
            response->set_term(current_term);
            response->set_success(false);
            return Status::OK;
        }

        // Consistency check: reject if our log doesn't match what the leader expects.
        // The leader will back off and retry with an earlier index.
        if (prev_index >= 0)
        {
            const bool snapshot_matches = prev_index == snapshot_last_index && prev_term == snapshot_last_term;
            const bool log_matches = prev_index >= log_offset &&
                                     prev_index < log_offset + (int)log_entries.size() &&
                                     log_entries[prev_index - log_offset].term() == prev_term;
            if (!snapshot_matches && !log_matches)
            {
                response->set_term(current_term);
                response->set_success(false);
                return Status::OK;
            }
        }

        // Preserve matching entries and their suffix; truncate only at the
        // first conflicting entry, as required by Raft's log-matching rule.
        const auto original_entries = log_entries;
        bool log_changed = false;
        int entry_index = prev_index + 1;
        for (const auto &entry : request->entries())
        {
            const int entry_offset = entry_index - log_offset;
            if (entry_offset >= 0 && entry_offset < static_cast<int>(log_entries.size()))
            {
                if (log_entries[entry_offset].term() == entry.term())
                {
                    ++entry_index;
                    continue;
                }
                if (entry_index <= commit_index)
                {
                    response->set_term(current_term);
                    response->set_success(false);
                    return Status::OK;
                }
                log_entries.resize(entry_offset);
            }
            LogEntry indexed_entry = entry;
            indexed_entry.set_log_index_plus_one(static_cast<std::uint64_t>(log_offset + static_cast<int>(log_entries.size())) + 1);
            log_entries.push_back(indexed_entry);
            log_changed = true;
            ++entry_index;
        }
        if (log_changed)
            RewriteWal();

        if (request->leader_commit() > commit_index)
        {
            const int last_log_index = log_offset + (int)log_entries.size() - 1;
            commit_index = std::min(request->leader_commit(),
                                    std::max(snapshot_last_index, last_log_index));
        }
        ApplyCommitted();

        response->set_term(current_term);
        response->set_success(true);
        response->set_match_index(std::max(snapshot_last_index,
                                           prev_index + request->entries_size()));
        return Status::OK;
    }

    Status InstallSnapshot(ServerContext *context, const kvstore::InstallSnapshotRequest *request, kvstore::InstallSnapshotResponse *response) override
    {
        RpcMetricTimer metric(*this, "InstallSnapshot");
        const auto authorization = Authorize(context, "node", "InstallSnapshot");
        if (!authorization.ok())
            return authorization;
        if (request->leader_id() != node_id && !IsConfiguredPeer(request->leader_id()))
            return RpcError("InstallSnapshot", grpc::StatusCode::PERMISSION_DENIED,
                            "snapshot sender is not a configured peer");
        if (request->leader_term() < 0 || request->last_included_index() < 0 ||
            request->last_included_index() == std::numeric_limits<int>::max() ||
            request->last_included_term() < 0 ||
            request->last_included_term() > request->leader_term())
            return RpcError("InstallSnapshot", grpc::StatusCode::INVALID_ARGUMENT,
                            "snapshot contains invalid Raft metadata");

        SnapshotState snapshot;
        try
        {
            snapshot = ParseSnapshotPayload(request->snapshot_data());
        }
        catch (const std::exception &error)
        {
            return RpcError("InstallSnapshot", grpc::StatusCode::INVALID_ARGUMENT, error.what());
        }

        {
            std::lock_guard<std::mutex> lock(election_mutex);
            if (request->leader_term() < current_term)
            {
                response->set_term(current_term);
                response->set_success(false);
                response->set_match_index(snapshot_last_index);
                return Status::OK;
            }
            if (request->leader_term() > current_term)
            {
                current_term = request->leader_term();
                voted_for = -1;
                try
                {
                    PersistMetadata();
                }
                catch (const std::exception &error)
                {
                    return RpcError("InstallSnapshot", grpc::StatusCode::INTERNAL, error.what());
                }
            }
            state = NodeState::FOLLOWER;
            leader_ready = false;
            known_leader_id = request->leader_id();
        }

        {
            std::lock_guard<std::mutex> lock(log_mutex);
            if (request->last_included_index() <= snapshot_last_index ||
                request->last_included_index() < commit_index)
            {
                response->set_term(current_term);
                response->set_success(request->last_included_index() <= snapshot_last_index);
                response->set_match_index(snapshot_last_index);
                return Status::OK;
            }

            const int included_index = request->last_included_index();
            const int included_offset = included_index - log_offset;
            const bool suffix_matches = included_offset >= 0 &&
                included_offset < static_cast<int>(log_entries.size()) &&
                log_entries[included_offset].term() == request->last_included_term();
            try
            {
                PersistSnapshotState(snapshot.store, snapshot.completed_requests, snapshot.client_sequences,
                                     included_index, request->last_included_term());
            }
            catch (const std::exception &error)
            {
                return RpcError("InstallSnapshot", grpc::StatusCode::INTERNAL, error.what());
            }
            if (suffix_matches)
                log_entries.erase(log_entries.begin(), log_entries.begin() + included_offset + 1);
            else
                log_entries.clear();
            log_offset = included_index + 1;
            snapshot_last_index = included_index;
            snapshot_last_term = request->last_included_term();
            commit_index = std::max(commit_index, snapshot_last_index);
            last_applied = std::max(last_applied, snapshot_last_index);
        }

        {
            std::lock_guard<std::mutex> store_lock(store_mutex);
            store = std::move(snapshot.store);
        }
        {
            std::lock_guard<std::mutex> request_lock(request_cache_mutex);
            completed_write_requests = std::move(snapshot.completed_requests);
            completed_client_sequences = std::move(snapshot.client_sequences);
        }
        try
        {
            RewriteWal();
        }
        catch (const std::exception &error)
        {
            return RpcError("InstallSnapshot", grpc::StatusCode::INTERNAL, error.what());
        }

        response->set_term(current_term);
        response->set_success(true);
        response->set_match_index(snapshot_last_index);
        return Status::OK;
    }

    // Handles a vote request from a candidate.
    Status RequestVote(ServerContext *context, const VoteRequest *request, VoteResponse *response) override
    {
        RpcMetricTimer metric(*this, "RequestVote");
        const auto authorization = Authorize(context, "node", "RequestVote");
        if (!authorization.ok())
            return authorization;
        if (request->candidate_id() != node_id && !IsConfiguredPeer(request->candidate_id()))
        {
            response->set_vote_granted(false);
            response->set_voter_term(current_term);
            return Status::OK;
        }
        std::lock_guard<std::mutex> lock(election_mutex);
        int last_log_index;
        int last_log_term;
        {
            std::lock_guard<std::mutex> log_lock(log_mutex);
            last_log_index = log_offset + static_cast<int>(log_entries.size()) - 1;
            last_log_term = log_entries.empty() ? snapshot_last_term : log_entries.back().term();
        }
        const bool candidate_log_is_up_to_date =
            request->candidate_last_log_term() > last_log_term ||
            (request->candidate_last_log_term() == last_log_term &&
             request->candidate_last_log_index() >= last_log_index);

        if (request->pre_vote())
        {
            std::chrono::steady_clock::time_point last_seen;
            {
                std::lock_guard<std::mutex> heartbeat_lock(heartbeat_mutex);
                last_seen = last_heartbeat;
            }
            const bool leader_contact_is_stale =
                std::chrono::steady_clock::now() - last_seen >= std::chrono::milliseconds(1500);
            const bool grant_pre_vote =
                request->candidate_term() > current_term &&
                candidate_log_is_up_to_date &&
                state != NodeState::LEADER &&
                leader_contact_is_stale;
            response->set_vote_granted(grant_pre_vote);
            response->set_voter_term(current_term);
            return Status::OK;
        }

        if (request->candidate_term() > current_term)
        {
            current_term = request->candidate_term();
            voted_for = -1;
            PersistMetadata();
            state = NodeState::FOLLOWER;
            leader_ready = false;
        }

        bool grant = false;
        if (request->candidate_term() >= current_term &&
            candidate_log_is_up_to_date &&
            (voted_for == -1 || voted_for == request->candidate_id()))
        {
            grant = true;
            voted_for = request->candidate_id();
            PersistMetadata();
            std::lock_guard<std::mutex> hb_lock(heartbeat_mutex);
            last_heartbeat = std::chrono::steady_clock::now();
        }
        response->set_vote_granted(grant);
        response->set_voter_term(current_term);
        LogEvent("debug", 0, "raft_vote_request",
                 {{"candidate_id", std::to_string(request->candidate_id())},
                  {"term", std::to_string(request->candidate_term())},
                  {"granted", grant ? "true" : "false"}});
        return Status::OK;
    }

    Status Heartbeat(ServerContext *context, const HeartbeatRequest *request, HeartbeatResponse *response) override
    {
        RpcMetricTimer metric(*this, "Heartbeat");
        const auto authorization = Authorize(context, "node", "Heartbeat");
        if (!authorization.ok())
            return authorization;
        if (request->leader_port() != node_id && !IsConfiguredPeer(request->leader_port()))
            return RpcError("Heartbeat", grpc::StatusCode::PERMISSION_DENIED,
                            "heartbeat sender is not a configured peer");

        std::lock_guard<std::mutex> lock(election_mutex);
        if (request->leader_term() < current_term)
        {
            response->set_alive(true);
            response->set_term(current_term);
            return Status::OK;
        }
        if (request->leader_term() > current_term)
        {
            current_term = request->leader_term();
            voted_for = -1;
            try
            {
                PersistMetadata();
            }
            catch (const std::exception &error)
            {
                return RpcError("Heartbeat", grpc::StatusCode::INTERNAL, error.what());
            }
        }
        state = NodeState::FOLLOWER;
        leader_ready = false;
        known_leader_id = request->leader_port();
        std::lock_guard<std::mutex> hb_lock(heartbeat_mutex);
        last_heartbeat = std::chrono::steady_clock::now();
        response->set_alive(true);
        response->set_term(current_term);
        return Status::OK;
    }

    Status Get(ServerContext *context, const GetRequest *request, GetResponse *response) override
    {
        RpcMetricTimer metric(*this, "Get");
        const auto authorization = Authorize(context, "read", "Get", &request->key());
        if (!authorization.ok())
            return authorization;
        if (state == NodeState::LEADER && request->consistency() == kvstore::ReadConsistency::STRONG &&
            !ConfirmLinearizableRead())
            return RpcError("Get", grpc::StatusCode::UNAVAILABLE,
                            "leader cannot confirm a quorum for a linearizable read");

        if (state == NodeState::LEADER && !leader_ready)
            return RpcError("Get", grpc::StatusCode::UNAVAILABLE, "leader is not ready to serve requests");

        if (state != NodeState::LEADER && !peer_stubs.empty())
        {
            if (request->consistency() == kvstore::ReadConsistency::EVENTUAL)
            {
                std::lock_guard<std::mutex> lock(store_mutex);
                auto it = store.find(request->key());
                if (it != store.end())
                {
                    response->set_value(it->second);
                    response->set_found(true);
                }
                else
                {
                    response->set_found(false);
                }
                response->set_leader_id(known_leader_id);
                return Status::OK;
            }

            auto leader_stub = FindLeaderStub();
            if (leader_stub)
            {
                GetRequest forwarded = *request;
                GetResponse forwarded_response;
                ClientContext ctx;
                ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
                AddForwardedAuth(context, ctx);
                const auto status = leader_stub->Get(&ctx, forwarded, &forwarded_response);
                if (status.ok())
                {
                    *response = forwarded_response;
                    return Status::OK;
                }
            }
            return RpcError("Get", grpc::StatusCode::UNAVAILABLE,
                            "leader is unavailable for a linearizable read");
        }

        std::lock_guard<std::mutex> lock(store_mutex);
        auto it = store.find(request->key());
        if (it != store.end())
        {
            response->set_value(it->second);
            response->set_found(true);
        }
        else
        {
            response->set_found(false);
        }
        response->set_leader_id(node_id);
        return Status::OK;
    }

    Status ListKeys(ServerContext *context, const ListKeysRequest *request, ListKeysResponse *response) override
    {
        RpcMetricTimer metric(*this, "ListKeys");
        TokenPolicy key_policy;
        const auto authorization = Authorize(context, "read", "ListKeys", nullptr, &key_policy);
        if (!authorization.ok())
            return authorization;
        if (state == NodeState::LEADER && request->consistency() == kvstore::ReadConsistency::STRONG &&
            !ConfirmLinearizableRead())
            return RpcError("ListKeys", grpc::StatusCode::UNAVAILABLE,
                            "leader cannot confirm a quorum for a linearizable read");

        if (state == NodeState::LEADER && !leader_ready)
            return RpcError("ListKeys", grpc::StatusCode::UNAVAILABLE,
                            "leader is not ready to serve requests");

        if (state != NodeState::LEADER && !peer_stubs.empty())
        {
            if (request->consistency() == kvstore::ReadConsistency::EVENTUAL)
            {
                std::lock_guard<std::mutex> lock(store_mutex);
                std::vector<std::string> keys;
                keys.reserve(store.size());
                for (const auto &[key, value] : store)
                {
                    if (!PolicyAllowsKey(key_policy, "read", key))
                        continue;
                    keys.push_back(key);
                }
                std::sort(keys.begin(), keys.end());
                for (const auto &key : keys)
                {
                    response->add_keys(key);
                }
                response->set_leader_id(known_leader_id);
                return Status::OK;
            }

            auto leader_stub = FindLeaderStub();
            if (leader_stub)
            {
                ListKeysRequest forwarded = *request;
                ListKeysResponse forwarded_response;
                ClientContext ctx;
                ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
                AddForwardedAuth(context, ctx);
                const auto status = leader_stub->ListKeys(&ctx, forwarded, &forwarded_response);
                if (status.ok())
                {
                    *response = forwarded_response;
                    return Status::OK;
                }
            }
        }

        if (state != NodeState::LEADER && !peer_stubs.empty() &&
            request->consistency() == kvstore::ReadConsistency::STRONG)
            return RpcError("ListKeys", grpc::StatusCode::UNAVAILABLE,
                            "leader is unavailable for a linearizable read");

        std::lock_guard<std::mutex> lock(store_mutex);
        std::vector<std::string> keys;
        keys.reserve(store.size());
        for (const auto &[key, value] : store)
        {
            if (!PolicyAllowsKey(key_policy, "read", key))
                continue;
            keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());
        for (const auto &key : keys)
        {
            response->add_keys(key);
        }
        response->set_leader_id(node_id);
        return Status::OK;
    }

    Status GetClusterStatus(ServerContext *context, const ClusterStatusRequest *, ClusterStatusResponse *response) override
    {
        RpcMetricTimer metric(*this, "GetClusterStatus");
        const auto authorization = Authorize(context, "monitor", "GetClusterStatus");
        if (!authorization.ok())
            return authorization;
        const auto current_state = state.load();
        int term;
        int leader_id;
        {
            std::lock_guard<std::mutex> lock(election_mutex);
            term = current_term.load();
            leader_id = current_state == NodeState::LEADER ? node_id : known_leader_id;
        }
        int committed;
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            committed = commit_index;
        }
        response->set_node_id(node_id);
        response->set_current_term(term);
        response->set_commit_index(committed);
        response->set_is_leader(current_state == NodeState::LEADER && leader_ready.load());
        response->set_leader_id(leader_id);
        return Status::OK;
    }

    Status Set(ServerContext *context, const SetRequest *request, SetResponse *response) override
    {
        RpcMetricTimer metric(*this, "Set");
        const auto authorization = Authorize(context, "write", "Set", &request->key());
        if (!authorization.ok())
            return authorization;
        if (request->key().empty() || request->key().size() > kMaxKeyBytes ||
            request->value().size() > kMaxValueBytes ||
            request->request_id().size() > kMaxRequestIdBytes ||
            request->client_id().size() > kMaxRequestIdBytes ||
            (request->client_id().empty() != (request->request_sequence() == 0)))
            return RpcError("Set", grpc::StatusCode::INVALID_ARGUMENT,
                            "key/value/request ID exceeds configured limits");

        std::lock_guard<std::mutex> write_lock(client_write_mutex);
        const std::string identity = WriteIdentity(request->request_id(), request->client_id(),
                                                   request->request_sequence());
        WriteRequestResult replay_result;
        if (TryReplayRequest(identity, &replay_result))
        {
            response->set_success(replay_result.success);
            response->set_leader_id(replay_result.leader_id);
            response->set_request_id(request->request_id());
            response->set_client_id(request->client_id());
            response->set_request_sequence(request->request_sequence());
            return Status::OK;
        }
        if (IsStaleClientSequence(request->client_id(), request->request_sequence()))
            return RpcError("Set", grpc::StatusCode::FAILED_PRECONDITION,
                            "client request sequence is older than the committed session");

        if (state == NodeState::LEADER && !leader_ready)
        {
            response->set_success(false);
            response->set_leader_id(node_id);
            response->set_request_id(request->request_id());
            response->set_client_id(request->client_id());
            response->set_request_sequence(request->request_sequence());
            return Status::OK;
        }

        if (state != NodeState::LEADER && !peer_stubs.empty())
        {
            auto leader_stub = FindLeaderStub();
            if (leader_stub)
            {
                SetRequest forwarded = *request;
                SetResponse forwarded_response;
                ClientContext ctx;
                ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
                AddForwardedAuth(context, ctx);
                const auto status = leader_stub->Set(&ctx, forwarded, &forwarded_response);
                if (status.ok())
                {
                    if (!forwarded_response.success())
                        LogEvent("warn", 2, "forwarded_write_rejected",
                                 {{"node_id", std::to_string(node_id)}});
                    response->set_success(forwarded_response.success());
                    response->set_leader_id(forwarded_response.leader_id());
                    response->set_request_id(forwarded_response.request_id());
                    response->set_client_id(forwarded_response.client_id());
                    response->set_request_sequence(forwarded_response.request_sequence());
                    return Status::OK;
                }
                LogEvent("warn", 2, "forwarded_write_failed",
                         {{"node_id", std::to_string(node_id)},
                          {"error", status.error_message()}});
            }
            else
                LogEvent("warn", 2, "forwarded_write_leader_unavailable",
                         {{"node_id", std::to_string(node_id)}});
            response->set_success(false);
            response->set_leader_id(known_leader_id);
            response->set_request_id(request->request_id());
            response->set_client_id(request->client_id());
            response->set_request_sequence(request->request_sequence());
            return Status::OK;
        }

        LogEntry entry;
        entry.set_term(current_term);
        entry.set_key(request->key());
        entry.set_value(request->value());
        entry.set_request_id(request->request_id());
        entry.set_client_id(request->client_id());
        entry.set_request_sequence(request->request_sequence());
        const bool success = CommitEntry(entry);
        response->set_success(success);
        response->set_leader_id(node_id);
        response->set_request_id(request->request_id());
        response->set_client_id(request->client_id());
        response->set_request_sequence(request->request_sequence());
        return Status::OK;
    }

    Status Delete(ServerContext *context, const DeleteRequest *request, DeleteResponse *response) override
    {
        RpcMetricTimer metric(*this, "Delete");
        const auto authorization = Authorize(context, "write", "Delete", &request->key());
        if (!authorization.ok())
            return authorization;
        if (request->key().empty() || request->key().size() > kMaxKeyBytes ||
            request->request_id().size() > kMaxRequestIdBytes ||
            request->client_id().size() > kMaxRequestIdBytes ||
            (request->client_id().empty() != (request->request_sequence() == 0)))
            return RpcError("Delete", grpc::StatusCode::INVALID_ARGUMENT,
                            "key/request ID exceeds configured limits");

        std::lock_guard<std::mutex> write_lock(client_write_mutex);
        const std::string identity = WriteIdentity(request->request_id(), request->client_id(),
                                                   request->request_sequence());
        WriteRequestResult replay_result;
        if (TryReplayRequest(identity, &replay_result))
        {
            response->set_success(replay_result.success);
            response->set_leader_id(replay_result.leader_id);
            response->set_request_id(request->request_id());
            response->set_client_id(request->client_id());
            response->set_request_sequence(request->request_sequence());
            return Status::OK;
        }
        if (IsStaleClientSequence(request->client_id(), request->request_sequence()))
            return RpcError("Delete", grpc::StatusCode::FAILED_PRECONDITION,
                            "client request sequence is older than the committed session");

        if (state == NodeState::LEADER && !leader_ready)
        {
            response->set_success(false);
            response->set_leader_id(node_id);
            response->set_request_id(request->request_id());
            response->set_client_id(request->client_id());
            response->set_request_sequence(request->request_sequence());
            return Status::OK;
        }

        if (state != NodeState::LEADER && !peer_stubs.empty())
        {
            auto leader_stub = FindLeaderStub();
            if (leader_stub)
            {
                DeleteRequest forwarded = *request;
                DeleteResponse forwarded_response;
                ClientContext ctx;
                ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
                AddForwardedAuth(context, ctx);
                const auto status = leader_stub->Delete(&ctx, forwarded, &forwarded_response);
                if (status.ok())
                {
                    response->set_success(forwarded_response.success());
                    response->set_leader_id(forwarded_response.leader_id());
                    response->set_request_id(forwarded_response.request_id());
                    response->set_client_id(forwarded_response.client_id());
                    response->set_request_sequence(forwarded_response.request_sequence());
                    return Status::OK;
                }
            }
            response->set_success(false);
            response->set_leader_id(known_leader_id);
            response->set_request_id(request->request_id());
            response->set_client_id(request->client_id());
            response->set_request_sequence(request->request_sequence());
            return Status::OK;
        }

        LogEntry entry;
        entry.set_term(current_term);
        entry.set_key(request->key());
        entry.set_value(kDeleteMarker);
        entry.set_request_id(request->request_id());
        entry.set_client_id(request->client_id());
        entry.set_request_sequence(request->request_sequence());
        const bool success = CommitEntry(entry);
        response->set_success(success);
        response->set_leader_id(node_id);
        response->set_request_id(request->request_id());
        response->set_client_id(request->client_id());
        response->set_request_sequence(request->request_sequence());
        return Status::OK;
    }
};

class MetricsHttpServer
{
public:
    ~MetricsHttpServer()
    {
        Stop();
    }

    void Start(const std::string &bind_address, const std::string &port,
               const KVStoreServiceImpl &service)
    {
        const int port_number = ParsePortNumber(port, "metrics");
        const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
        if (descriptor < 0)
            throw std::runtime_error("failed to create metrics listener socket");

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port_number));
        if (inet_pton(AF_INET, bind_address.c_str(), &address.sin_addr) != 1)
        {
            close(descriptor);
            throw std::runtime_error("metrics_bind must be an IPv4 address");
        }
        int reuse_address = 1;
        if (setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR,
                       &reuse_address, sizeof(reuse_address)) < 0)
        {
            const int error = errno;
            close(descriptor);
            throw std::runtime_error("failed to configure metrics listener: " +
                                     std::string(std::strerror(error)));
        }
        if (bind(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
            listen(descriptor, 16) < 0)
        {
            const int error = errno;
            close(descriptor);
            throw std::runtime_error("failed to start metrics listener: " +
                                     std::string(std::strerror(error)));
        }
        listener_.store(descriptor);
        running_ = true;
        worker_ = std::thread([this, &service]() { Serve(service); });
        LogEvent("info", 1, "metrics_listener_started",
                 {{"bind", bind_address}, {"port", port}});
    }

    void Stop()
    {
        running_ = false;
        const int descriptor = listener_.exchange(-1);
        if (descriptor >= 0)
        {
            shutdown(descriptor, SHUT_RDWR);
            close(descriptor);
        }
        if (worker_.joinable())
            worker_.join();
    }

private:
    std::atomic<bool> running_{false};
    std::atomic<int> listener_{-1};
    std::thread worker_;

    static void SendResponse(int descriptor, int status, const std::string &body)
    {
        const std::string reason = status == 200 ? "OK" : "Not Found";
        const std::string response =
            "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
            "Content-Type: " + (status == 200 ? "text/plain; version=0.0.4; charset=utf-8" :
                                               "text/plain; charset=utf-8") + "\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + body;
        std::size_t sent = 0;
        while (sent < response.size())
        {
#ifdef MSG_NOSIGNAL
            const auto count = send(descriptor, response.data() + sent,
                                    response.size() - sent, MSG_NOSIGNAL);
#else
            const auto count = send(descriptor, response.data() + sent,
                                    response.size() - sent, 0);
#endif
            if (count <= 0)
                return;
            sent += static_cast<std::size_t>(count);
        }
    }

    void Serve(const KVStoreServiceImpl &service)
    {
        while (running_)
        {
            const int listener = listener_.load();
            if (listener < 0)
                break;
            const int client = accept(listener, nullptr, nullptr);
            if (client < 0)
            {
                if (!running_ || errno == EINTR)
                    continue;
                LogEvent("error", 3, "metrics_accept_failed",
                         {{"error", std::strerror(errno)}});
                continue;
            }
            timeval timeout{1, 0};
            if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
                setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0)
            {
                LogEvent("warn", 2, "metrics_client_timeout_setup_failed",
                         {{"error", std::strerror(errno)}});
                close(client);
                continue;
            }
            char request[1024]{};
            const auto received = recv(client, request, sizeof(request) - 1, 0);
            const std::string request_line = received > 0 ? std::string(request, received) : "";
            if (request_line.rfind("GET /metrics HTTP/1.", 0) == 0)
                SendResponse(client, 200, service.RenderMetrics());
            else
                SendResponse(client, 404, "not found\n");
            close(client);
        }
    }
};

bool RunServer(const std::string &bind_address, const std::string &port,
               const std::string &metrics_bind, const std::string &metrics_port,
               const std::vector<std::string> &peer_addresses,
               const std::vector<int> &peer_ids, const TlsConfig &tls_config,
               const std::filesystem::path &membership_path,
               const std::filesystem::path &data_root)
{
    const std::string server_address =
        (bind_address == "::" || bind_address == "[::]")
            ? "[::]:" + port
            : bind_address + ":" + port;
    KVStoreServiceImpl service(std::stoi(port), peer_addresses, peer_ids,
                               data_root / ("node_" + port), tls_config,
                               membership_path);
    MetricsHttpServer metrics_http;
    if (!metrics_port.empty())
        metrics_http.Start(metrics_bind, metrics_port, service);

    int shutdown_pipe[2];
    if (pipe(shutdown_pipe) != 0)
        throw std::runtime_error("failed to create graceful-shutdown signal pipe");
    for (const int descriptor : shutdown_pipe)
    {
        const int flags = fcntl(descriptor, F_GETFL, 0);
        if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0)
        {
            close(shutdown_pipe[0]);
            close(shutdown_pipe[1]);
            throw std::runtime_error("failed to configure graceful-shutdown signal pipe");
        }
    }

    ServerBuilder builder;
    builder.AddChannelArgument(GRPC_ARG_MAX_CONCURRENT_STREAMS, 64);
    builder.SetMaxReceiveMessageSize(16 * 1024 * 1024);
    if (tls_config.Enabled())
    {
        builder.AddListeningPort(
            server_address,
            kvstore_credentials::CreateServerCredentials(
                tls_config.ca_file, tls_config.certificate_file,
                tls_config.private_key_file, tls_config.crl_directory));
    }
    else
    {
        builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    }
    builder.RegisterService(&service);

    std::unique_ptr<Server> server(builder.BuildAndStart());

    // If another process is already bound to this port, BuildAndStart()
    // returns nullptr instead of throwing. Fail loudly instead of crashing
    // on server->Wait() below.
    if (!server)
    {
        close(shutdown_pipe[0]);
        close(shutdown_pipe[1]);
        LogEvent("error", 3, "grpc_listener_start_failed",
                 {{"address", server_address}, {"reason", "port may already be in use"}});
        return false;
    }

    LogEvent("info", 1, "grpc_listener_started",
             {{"address", server_address}, {"node_id", port}});
    termination_signal = 0;
    shutdown_wakeup_fd = shutdown_pipe[1];
    std::signal(SIGINT, HandleTerminationSignal);
    std::signal(SIGTERM, HandleTerminationSignal);
    std::signal(SIGHUP, HandleTerminationSignal);
    std::atomic<bool> wait_complete{false};
    std::thread shutdown_watcher([&]() {
        pollfd wakeup{shutdown_pipe[0], POLLIN, 0};
        while (true)
        {
            const int poll_result = poll(&wakeup, 1, -1);
            if (poll_result < 0 && errno == EINTR)
                continue;
            if (poll_result < 0)
            {
                LogEvent("error", 3, "graceful_shutdown_wait_failed",
                         {{"error", std::strerror(errno)}});
                server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(10));
                return;
            }

            char signals[64];
            const auto count = read(shutdown_pipe[0], signals, sizeof(signals));
            if (count < 0 && (errno == EAGAIN || errno == EINTR))
                continue;
            if (count <= 0)
                return;
            for (ssize_t index = 0; index < count; ++index)
            {
                if (signals[index] == 'R')
                {
                    try
                    {
                        service.ReloadSecurityFiles();
                    }
                    catch (const std::exception &error)
                    {
                        LogEvent("error", 3, "security_credentials_reload_failed",
                                 {{"error", error.what()}});
                    }
                }
                else if (signals[index] == 'S')
                {
                    if (!wait_complete.load())
                    {
                        LogEvent("info", 1, "graceful_shutdown_started",
                                 {{"signal", std::to_string(termination_signal)}});
                        server->Shutdown(std::chrono::system_clock::now() +
                                         std::chrono::seconds(10));
                    }
                    return;
                }
                else if (signals[index] == 'Q')
                {
                    return;
                }
            }
        }
    });
    server->Wait();
    wait_complete = true;
    const char wakeup = 'Q';
    const auto ignored = write(shutdown_pipe[1], &wakeup, sizeof(wakeup));
    (void)ignored;
    shutdown_watcher.join();
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
    std::signal(SIGHUP, SIG_DFL);
    shutdown_wakeup_fd = -1;
    close(shutdown_pipe[0]);
    close(shutdown_pipe[1]);
    metrics_http.Stop();
    LogEvent("info", 1, "server_shutdown_complete", {{"node_id", port}});
    return true;
}

int main(int argc, char **argv)
{
    try
    {
        ConfigureLogLevel();
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << std::endl;
        return 2;
    }
    // Usage: server.exe <port> <peer1_port> <peer2_port> ... [options]
    // Or: server.exe --config node.conf
    if (argc < 2)
    {
        std::cout << "Usage: server.exe <port> <peer1_port> <peer2_port> ... [options]" << std::endl;
        return 1;
    }

    RuntimeConfig runtime_config;
    std::filesystem::path config_path;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::string(argv[i]) == "--config")
        {
            config_path = argv[i + 1];
            break;
        }
    }
    if (!config_path.empty())
        runtime_config = LoadRuntimeConfig(config_path);

    std::string port = runtime_config.port;
    std::string bind_address = runtime_config.bind;
    std::string metrics_port = runtime_config.metrics_port;
    std::string metrics_bind = runtime_config.metrics_bind;
    std::vector<std::string> peer_addresses;
    std::vector<int> peer_ids;
    TlsConfig tls_config;
    std::filesystem::path membership_path = runtime_config.peers_file;
    std::filesystem::path data_root = runtime_config.data_root;
    tls_config.ca_file = runtime_config.ca_file;
    tls_config.certificate_file = runtime_config.certificate_file;
    tls_config.private_key_file = runtime_config.private_key_file;
    tls_config.auth_file = runtime_config.auth_file.string();
    tls_config.token_file = runtime_config.token_file.string();
    tls_config.crl_directory = runtime_config.crl_directory.string();
    bool insecure_dev = false;
    bool invalid_peer_endpoint = false;

    auto add_peer = [&](const std::string &peer) {
        const auto separator = peer.rfind(':');
        const auto address = separator == std::string::npos ? "localhost:" + peer : peer;
        try
        {
            const auto port_text = separator == std::string::npos ? peer : peer.substr(separator + 1);
            peer_ids.push_back(ParsePortNumber(port_text, "peer"));
            peer_addresses.push_back(address);
        }
        catch (const std::exception &error)
        {
            std::cerr << "Invalid peer endpoint '" << peer << "': " << error.what() << std::endl;
            invalid_peer_endpoint = true;
        }
    };
    for (const auto &peer : runtime_config.peers)
        add_peer(peer);

    int argument_index = 1;
    if (std::string(argv[1]) != "--config")
    {
        port = argv[1];
        argument_index = 2;
    }
    if (port.empty())
    {
        std::cerr << "A port is required either as an argument or in --config." << std::endl;
        return 1;
    }

    for (int i = argument_index; i < argc; i++)
    {
        if (std::string(argv[i]) == "--config" && i + 1 < argc)
        {
            ++i;
            continue;
        }
        if (std::string(argv[i]) == "--data-root" && i + 1 < argc)
        {
            data_root = argv[++i];
            continue;
        }
        if (std::string(argv[i]) == "--bind" && i + 1 < argc)
        {
            bind_address = argv[++i];
            continue;
        }
        if (std::string(argv[i]) == "--metrics-port" && i + 1 < argc)
        {
            metrics_port = argv[++i];
            continue;
        }
        if (std::string(argv[i]) == "--metrics-bind" && i + 1 < argc)
        {
            metrics_bind = argv[++i];
            continue;
        }
        if (std::string(argv[i]) == "--auth-file" && i + 1 < argc)
        {
            tls_config.auth_file = argv[++i];
            continue;
        }
        if (std::string(argv[i]) == "--token-file" && i + 1 < argc)
        {
            tls_config.token_file = argv[++i];
            continue;
        }
        if (std::string(argv[i]) == "--crl-directory" && i + 1 < argc)
        {
            tls_config.crl_directory = argv[++i];
            continue;
        }
        if (std::string(argv[i]) == "--insecure-dev")
        {
            insecure_dev = true;
            continue;
        }
        if (std::string(argv[i]) == "--ca" && i + 1 < argc)
        {
            tls_config.ca_file = argv[++i];
            continue;
        }
        if (std::string(argv[i]) == "--cert" && i + 1 < argc)
        {
            tls_config.certificate_file = argv[++i];
            continue;
        }
        if (std::string(argv[i]) == "--key" && i + 1 < argc)
        {
            tls_config.private_key_file = argv[++i];
            continue;
        }
        if (std::string(argv[i]) == "--peers-file" && i + 1 < argc)
        {
            membership_path = argv[++i];
            continue;
        }
        add_peer(argv[i]);
    }

    if (invalid_peer_endpoint)
        return 2;

    if (tls_config.Configured() && !tls_config.Enabled())
    {
        std::cerr << "TLS requires --ca, --cert, and --key to be configured together." << std::endl;
        return 2;
    }

    if (tls_config.auth_file.empty() != tls_config.token_file.empty())
    {
        std::cerr << "--auth-file and --token-file must be configured together." << std::endl;
        return 2;
    }
    if (!tls_config.Enabled() && !tls_config.crl_directory.empty())
    {
        std::cerr << "--crl-directory requires TLS credentials." << std::endl;
        return 2;
    }

    const bool loopback_bind = bind_address == "localhost" || bind_address == "127.0.0.1" ||
                               bind_address == "::1" || bind_address == "[::1]";
    if (insecure_dev)
    {
        if (!loopback_bind)
        {
            std::cerr << "--insecure-dev is restricted to loopback binds." << std::endl;
            return 2;
        }
    }
    else if (!tls_config.Enabled() || tls_config.auth_file.empty())
    {
        std::cerr << "TLS/mTLS and RPC authorization are required; use --insecure-dev only for local development."
                  << std::endl;
        return 2;
    }

    RuntimeConfig effective_config = runtime_config;
    effective_config.port = port;
    effective_config.bind = bind_address;
    effective_config.metrics_port = metrics_port;
    effective_config.metrics_bind = metrics_bind;
    effective_config.peers = peer_addresses;
    effective_config.ca_file = tls_config.ca_file;
    effective_config.certificate_file = tls_config.certificate_file;
    effective_config.private_key_file = tls_config.private_key_file;
    effective_config.crl_directory = tls_config.crl_directory;
    effective_config.auth_file = tls_config.auth_file;
    effective_config.token_file = tls_config.token_file;
    try
    {
        ValidateRuntimeConfig(effective_config);
    }
    catch (const std::exception &error)
    {
        std::cerr << "Invalid server configuration: " << error.what() << std::endl;
        return 2;
    }

    if (tls_config.Enabled())
        LogEvent("info", 1, "transport_security_enabled");
    try
    {
        return RunServer(bind_address, port, metrics_bind, metrics_port, peer_addresses, peer_ids, tls_config,
                         membership_path, data_root) ? 0 : 1;
    }
    catch (const std::exception &error)
    {
        LogEvent("error", 3, "server_startup_failed", {{"error", error.what()}});
        return 1;
    }
}