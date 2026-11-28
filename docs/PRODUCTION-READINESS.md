# Production Readiness

## Current status

This repository is an experimental distributed key-value store and is **not yet ready for production data**. It has a runnable C++/gRPC Raft cluster, persistent log and metadata, snapshots, a browser gateway, mTLS and role-authorized gRPC access for secured deployments, and automated tests. Those features are a starting point, not evidence of production-grade safety, availability, or support.

## Progress tracker

✅ means the specific documentation or code change is present. It does **not** mean the production capability has been independently validated. ⬜ means work remains.

## Working deployment profile

The project's scope is controlled internal experimentation, not a public release or a
production database. Until a release profile is measured and approved, use only disposable
or independently backed-up data.

The conservative topology candidate is a static three-node cluster in one private
datacenter; five nodes are an unqualified option, not a measured recommendation. Every
node must be able to reach every configured peer. The gRPC listener defaults to loopback
and requires mTLS plus RPC role tokens; use private bind addresses and firewall rules for
remote peers. Plaintext is available only through the explicit loopback development mode.
Do not expose demo configuration to the public internet.

The code uses POSIX file APIs (`fsync`, directory `fsync`, and atomic rename) for durable
state replacement. A candidate environment is Linux or macOS with a local filesystem
that honors those operations. No OS release, filesystem, network filesystem, container
volume, or storage device has been qualified; native Windows is not a supported target
for this source until its POSIX persistence and process APIs are ported.

There is no supported rolling upgrade or downgrade procedure. Keep all nodes on the same
build. Before changing versions, stop the whole cluster and preserve an off-host copy of
the complete data root. Do not roll back binaries against data written by a newer build;
restore only by a separately rehearsed procedure on a disposable cluster.

The server enforces per-operation limits of 4 KiB per key, 1 MiB per value, and 256 bytes
per request ID or client ID. These are input limits, not a capacity claim. There is no
configured total-keyspace, on-disk-size, or write-rate limit. The benchmark now supports concurrent set/get/mixed workloads and reports operation
latency percentiles, but no supported throughput, latency, or capacity target is
published.

Successful writes are intended to mean local WAL synchronization plus acknowledgement
from a majority of the static membership. Strong reads use a same-term quorum check;
eventual reads may be stale without a time bound. These are descriptions of the current
implementation, not independently verified guarantees. No availability SLO, RTO, or
RPO is established.

### Phase 1 controlled-experimentation envelope

For this repository phase, the only supported deployment claim is the controlled
internal-experimentation profile described above. It is not a capacity-qualified
deployment and must not use irreplaceable data.

- **Workload and storage:** the published limits are 4 KiB per key, 1 MiB per value,
  256 bytes per request or client ID, and no aggregate keyspace or write-rate limit.
  The absence of aggregate limits means the operator has not been given a validated
  storage, memory, or throughput ceiling. Benchmark results must include their exact
  cluster, storage, network, build, and run conditions; they do not increase this envelope.
- **Consistency:** successful writes use the implemented majority-acknowledged Raft
  commit path. Strong reads use the implemented same-term quorum check. Eventual reads
  may observe local, stale state with no bounded lag. These behaviors are
  implementation descriptions, not independently verified production guarantees.
- **Availability:** no availability percentage or uptime SLO is established. Leader
  election and failover have been covered only by repository tests and are not a
  measured availability target.
- **Recovery objectives:** RTO and RPO are not established. Restart, restore, node
  replacement, and crash or power-loss recovery remain unverified operational exercises.
  The only conservative RPO is an independent off-host backup; the only conservative
  RTO is to hold no deadline for service restoration.

This envelope completes Phase 1 by making the exact, limited claim explicit. It does
not authorize production use or change the remaining validation work in later phases.

### Completed in this repository

- ✅ Drafted an initial, conservative deployment profile: static 3- or 5-node clusters, private single-datacenter network, and controlled internal-use scope. This profile is not yet measured, approved, or a production support guarantee.
- ✅ Documented that reloading the peer file is not a safe Raft voting-membership change; static membership remains the only supported mode.
- ✅ Gateway now defaults to loopback binding. Public binding requires explicit opt-in and a configured users file; regression tests cover these checks.
- ✅ Dashboard account authentication, per-user key isolation, and password-file permissions are implemented and covered by gateway tests. Direct gRPC additionally requires mTLS and scoped tokens.
- ✅ mTLS and role-token authorization are required for normal server/client/gateway operation; the local demo opts into loopback-only plaintext development mode explicitly.
- ✅ Partial TLS/auth configuration is rejected rather than silently falling back to plaintext; bind failures return a failing process exit code.
- ✅ The gateway exposes health/readiness endpoints, HTTP counters/latency histograms, and node Prometheus metrics cover Raft state, replication lag, WAL/snapshot activity, RPC latency/errors, process resource use, and storage file size.
- ✅ The server enforces per-item input limits of 4 KiB per key, 1 MiB per value, and 256 bytes per request/client ID. These limits are not workload or capacity targets.

### Phase 1: Supported deployment profile — complete for the controlled-experimentation scope

The working scope and conservative assumptions above are documented, but they are not a
qualified support contract. Remaining work:

- ✅ Established an explicit controlled-experimentation workload boundary: per-item limits are documented, aggregate storage and write-rate limits are not claimed, and benchmark conditions are required. This is not a measured production capacity.
- ✅ Defined the current consistency behavior and explicitly set availability, RTO, and RPO targets to "not established" rather than claiming unverified numbers.
- ✅ Select controlled internal experimentation as the repository's scope; this is not approval for production or public use.
- ✅ Document candidate platform, storage, network, and upgrade assumptions; exact versions and storage configurations remain unqualified.

### Phase 2: Raft safety and recovery — incomplete

Repository-owned regression coverage now exercises log matching, stale-vote rejection,
leader quorum loss, snapshot installation, snapshot compaction, checksum-backed WAL
records, corrupt snapshot metadata, invalid metadata, truncated WAL tails, and repeated
single-node restarts. These checks reduce known risk; they do not replace a formal Raft
safety audit, power-loss qualification, disk-I/O fault testing, partition testing, or
independent consensus/persistence review.

- ✅ Added repository regression coverage for term rejection, stale log voting, log matching, leader quorum loss, commit readiness, and snapshot boundaries. An independent safety audit remains required.
- ✅ Keep voting membership static and clearly identify peer-file reload as endpoint configuration only; safe membership changes are not implemented.
- ✅ Reject malformed AppendEntries term/index ranges and invalid snapshot Raft metadata; focused regression cases are included and can be run with the C++ suite.
- ⬜ Test partitions, delayed/reordered messages, disk-full/I/O errors, and stale/lagging node recovery; corrupt metadata/snapshots and repeated restarts now have focused repository cases.
- ⬜ Verify acknowledged-write durability for the explicitly supported crash and power-loss model.
- ⬜ Add long-running multi-node soak tests and obtain independent consensus/persistence review.

### Phase 3: Security — implemented for controlled experimentation; external qualification open

The code now provides optional CRL enforcement, reloadable token policy, staged
credential-bundle rotation, and per-key byte-prefix scopes. These controls are not a
production security certification. Only trusted operators should issue credentials or
expose network listeners. See the security setup and lifecycle procedure in
[the security setup guide](SECURITY.md).

- ✅ Authenticate every implemented direct gRPC operation with a bearer token; TLS/mTLS is required by default.
- ✅ Apply separate `read`, `write`, `monitor`, and `node` roles. Node credentials cannot call key-value RPCs; peer forwarding preserves the caller's role. Dashboard backup/restore uses its gateway's explicit read/write identity and remains account-namespaced.
- ✅ Require mTLS and RPC authorization for normal server, C++ client, and gateway startup; plaintext is an explicit loopback-only development mode.
- ✅ Default node listeners to loopback; a non-loopback listener requires explicit configuration and TLS/mTLS.
- ✅ Support optional gRPC CRL-directory enforcement for C++ server/client; verify revoked client-certificate rejection and document the Python gateway limitation, hashed CRL files, and restart requirements.
- ✅ Reload validated token/role policy on SIGHUP without restarting Raft state; reject the replacement atomically if the file is invalid.
- ✅ Provide an atomic staged credential-bundle switch helper, per-call token-file reload for C++/gateway clients, and an integration regression for token revocation.
- ✅ Enforce optional literal byte-prefix read/write scopes for `Get`, `ListKeys`, `Set`, and `Delete`; legacy tokens without scopes remain global for compatibility.
- ✅ Document private credential file permissions, provisioning, certificate/token rotation, expiry handling, and emergency replacement.
- ✅ Make gateway loopback binding the default; require explicit opt-in and configured dashboard authentication for non-loopback/public binding.
- ✅ Review declared dependency/configuration boundaries and document the threat model; configure a per-connection gRPC stream limit and a 16 MiB inbound message ceiling; test unauthenticated access, role violations, and oversize request rejection. No dependency lockfile or independent upstream security audit is present.
- ✅ Document credential ownership, approval, storage, and replacement handling.

Token policy reload and file-based credential rotation require explicit operator
coordination and verification; the helper cannot prove that every remote node and
gateway accepted a rotation. CRL enforcement depends on the gRPC/OpenSSL build and
changing CRL files requires process restart. Unscoped tokens still address the global
keyspace. There is no separately privileged backup RPC. Independent security review and
production credential approval remain deployment gates; this is not a claim that the
system is safe for production data.

### Phase 4: Operations and observability — complete for the controlled-experimentation implementation

The implementation and runbooks now provide operator hooks and candidate procedures; they
do not establish production thresholds, capacity, recovery targets, or a supported release.

- ✅ Emit JSON-line node/gateway logs with configurable severity, Raft transition and persistence events, and no key/value data in node apply logs.
- ✅ Provide optional loopback-by-default Prometheus node metrics, gateway HTTP count/latency metrics, readiness/health definitions, SIGTERM graceful shutdown, and an external restart-policy example.
- ✅ Document candidate alerts, capacity limitations, off-host backup/retention, restore drills, disaster recovery, migration, and rollback constraints.
- ✅ Provide CMake install/TGZ packaging, a node config example, and repeatable deployment-build guidance. Credentials/data are intentionally excluded from packages.

See the [operations runbook](OPERATIONS.md) for scraping and alerting, backup/restore
limitations, deployment controls, migration and restart procedures. Production thresholds,
backup retention approval, restore qualification, and signed release artifacts remain
operator/release gates.

### Phase 5: Performance and release validation — repository gates implemented; qualification open

- ✅ Add a concurrent read/write/mixed-load benchmark with size, worker, latency-percentile, throughput, and explicit topology-label output.
- ✅ Add a repeatable benchmark matrix runner that records commit/platform metadata and optional node storage/CPU/RSS metric snapshots.
- ✅ Add a deterministic paused-leader failover/rejoin regression; CI also runs the existing Raft and persistence integration tests.
- ✅ Add clean C++/Python test, ASan/UBSan, Python dependency-audit, and macOS build/test workflow gates.
- ✅ Publish known compatibility limits, version/tag policy, repository release gates, and external qualification gates in the [release process](RELEASE-PROCESS.md).
- ⬜ Establish a reliable TSan gate against instrumented gRPC/Abseil dependencies. CMake now supports an explicit gRPC source checkout so sanitizer flags also cover those dependencies; this configuration has not been built or run. The available distro libraries produced reports inside uninstrumented runtime internals; TSan also exposed an application race in cluster-status reads, which was fixed.
- ⬜ Measure representative workloads and capacity across approved cluster sizes and target storage/network profiles; local benchmark output is not a capacity claim.
- ⬜ Complete partition, delay/reordering, disk-full/I/O, crash/power-loss, restore, and node-replacement qualification in the intended environment.
- ⬜ Obtain independent consensus/persistence/security review, staging soak, restore sign-off, and approved operational targets.

## Before any trial deployment

- ⬜ Use only disposable or fully backed-up data until recovery behavior has been independently validated.
- ⬜ Restrict node and gateway ports to a trusted private network; do not expose unauthenticated gRPC or demo gateway mode publicly.
- ✅ Keep cluster voting membership static; do not treat peer-file reload as a live membership change.
- ⬜ Test backup restoration and node replacement using the exact deployment configuration; the documented drill is not yet qualified.