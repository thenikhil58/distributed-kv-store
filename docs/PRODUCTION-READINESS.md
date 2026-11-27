# Production Readiness

## Current status

This repository is an experimental distributed key-value store and is **not yet ready for production data**. It has a runnable C++/gRPC Raft cluster, persistent log and metadata, snapshots, a browser gateway, optional mutual TLS, and automated tests. Those features are a starting point, not evidence of production-grade safety, availability, or support.

## Progress tracker

✅ means the specific documentation or code change is present. It does **not** mean the production capability has been independently validated. ⬜ means work remains.

## Working deployment profile

The project's scope is controlled internal experimentation, not a public release or a
production database. Until a release profile is measured and approved, use only disposable
or independently backed-up data.

The conservative topology candidate is a static three-node cluster in one private
datacenter; five nodes are an unqualified option, not a measured recommendation. Every
node must be able to reach every configured peer. Keep node ports behind private-network
firewall rules: the gRPC server listens on all interfaces, and direct gRPC access is not
authenticated when mTLS is omitted. Do not expose this configuration to the public
internet.

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
configured total-keyspace, on-disk-size, or write-rate limit. The benchmark currently
reports sequential write throughput only; no supported throughput, latency, or capacity
target is published.

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
- ✅ Dashboard account authentication, per-user key isolation, and password-file permissions are implemented and covered by gateway tests. This does not authenticate direct gRPC clients.
- ✅ mTLS options are implemented for server/client/gateway configuration, but mTLS is optional and is not yet enforced as a deployment default.
- ✅ Partial server TLS configuration is rejected rather than silently falling back to plaintext; bind failures now return a failing process exit code. This does not change the default insecure gRPC mode.
- ✅ The gateway exposes health/readiness endpoints and Prometheus counters for gateway operations and HTTP method/status pairs. These do not provide Raft-node metrics, latency histograms, or resource measurements.
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

### Phase 3: Security — incomplete

- ⬜ Authenticate and authorize direct gRPC clients; dashboard login does not protect direct gRPC access.
- ⬜ Implement and verify least-privilege access for reads, writes, administration, backups, and restores.
- ⬜ Require TLS/mTLS by default for supported deployments and define safe node bind/network defaults.
- ⬜ Document and validate certificate/secret provisioning, rotation, revocation, expiry, and emergency replacement.
- ✅ Make gateway loopback binding the default; require explicit opt-in and configured dashboard authentication for non-loopback/public binding.
- ⬜ Complete dependency/configuration/threat-model review and test abuse, resource exhaustion, authentication failures, and connection limits.
- ⬜ Establish approval and handling procedures for production credentials and configuration.

### Phase 4: Operations and observability — incomplete

- ⬜ Add structured, configurable logs and production metrics for Raft state, replication lag, WAL/snapshot activity, request latency/errors, and resource use.
- ⬜ Define alerts, health/readiness behavior, graceful shutdown, restart policy, and capacity guidance.
- ⬜ Document backup retention, off-host storage, restore drills, disaster recovery, and migration procedures.
- ⬜ Provide reproducible packaging/deployment, configuration examples, and an upgrade/rollback procedure.

### Phase 5: Performance and release validation — not started

- ⬜ Run representative load, latency-percentile, capacity, and resource-consumption tests across supported cluster sizes.
- ⬜ Run repeatable fault-injection and recovery tests in CI or a dedicated staging environment.
- ⬜ Establish release gates for clean builds, C++/Python tests, sanitizers, security checks, restore drills, and staging soak tests.
- ⬜ Publish known limitations, compatibility policy, operational runbooks, and a versioned release process.

## Before any trial deployment

- ⬜ Use only disposable or fully backed-up data until recovery behavior has been independently validated.
- ⬜ Restrict node and gateway ports to a trusted private network; do not expose unauthenticated gRPC or demo gateway mode publicly.
- ✅ Keep cluster voting membership static; do not treat peer-file reload as a live membership change.
- ⬜ Test backup restoration and node replacement using the exact deployment configuration.