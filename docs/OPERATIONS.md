# Operations Runbook

The project is suitable only for controlled experimentation; no production SLO, capacity
limit, RTO, or RPO is established. Use disposable data or an independently recoverable
backup. This guide describes operational controls and the current behavior, not a support
guarantee.

## Health, readiness, and alerts

- `GET /health` and `GET /api/health` on the gateway check whether the configured gRPC
  target answers `GetClusterStatus`. HTTP 200 means connected, not that the whole cluster
  has quorum or can commit writes.
- `GET /ready` returns 200 only when the gateway's target reports itself as the ready
  leader. It returns 503 for a follower, an unready leader, or an unreachable target.
  Point readiness checks at a known cluster target; do not use it as a liveness probe.
- Node Prometheus metrics are served from `http://<metrics_bind>:<metrics_port>/metrics`
  when `metrics_port` is configured. The default metrics bind is loopback. The endpoint
  is unauthenticated, so keep it loopback-only or firewall it to a trusted local scraper.
  The gateway's `/metrics` endpoint reports HTTP response counts and latency histograms.
- Set `KV_LOG_LEVEL` to `debug`, `info` (default), `warn`, or `error`. Server runtime logs
  are JSON lines; debug-level Raft-apply events do not include key values.

Suggested alerts should notify an operator rather than assume automatic recovery:

```promql
sum(raft_node_state{state="leader"}) != 1
max(raft_replication_lag_entries) > 0
increase(kvstore_rpc_errors_total[5m]) > 0
process_max_resident_memory_bytes > 2147483648
```

These are starting signals, not validated thresholds; the memory expression is an
example 2 GiB ceiling to replace with the host's approved budget. Configure the scraper to detect a
missing `up` target and use the host's disk-free metrics; this process does not set an
aggregate disk quota or publish a universal capacity threshold. Correlate alerts with
JSON `raft_leader_elected`, `raft_leader_lost_quorum`, `raft_log_compacted`,
`snapshot_persist_failed`, and `raft_wal_append_failed` events.

## Startup, restart, and graceful shutdown

Use one immutable build/configuration across the static cluster. Keep gRPC ports private,
use mTLS plus the node/client tokens described in [the security guide](SECURITY.md), and
run each process under a dedicated unprivileged account. Configure an external service
manager to restart on unexpected exit and to stop with SIGTERM; the C++ server stops
accepting RPCs with a ten-second gRPC shutdown grace period. Do not have a supervisor
restart a node repeatedly against a failing storage device; route persistent restart
failures to an operator.

Example systemd unit (replace paths and the account for the deployment):

```ini
[Unit]
Description=Raft KV node
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=raft-kv
Group=raft-kv
ExecStart=/opt/raft-kv/bin/server --config /etc/raft-kv/node.conf
Restart=on-failure
RestartSec=5s
TimeoutStopSec=15s
KillSignal=SIGTERM
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ReadWritePaths=/var/lib/raft-kv
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

Before a planned node restart, confirm the remaining static members have a leader and
quorum. Restart only one node at a time and verify it rejoins and catches up before
proceeding. `start.sh`/`stop.sh` are local demo helpers, not a cluster supervisor.

## Capacity and resource guidance

Each key is limited to 4 KiB, value to 1 MiB, and request/client ID to 256 bytes. gRPC
accepts messages up to 16 MiB and limits concurrent HTTP/2 streams to 64 per connection.
The gateway accepts request bodies up to 10 MiB and caps HTTP workers at 64. These are
per-operation/per-connection guardrails, not a cluster capacity profile. There is no
aggregate keyspace, disk, authenticated request-rate, total TCP-connection, or memory
limit.

Scrape each node's CPU time, maximum resident memory, Raft storage bytes, WAL activity,
snapshot activity, and replication lag. Establish workload and disk headroom empirically
on the exact filesystem, host, network, node count, build, and data set. Stop writes
before the disk is exhausted; disk-full behavior is not a supported capacity policy.
Avoid setting alert thresholds based on this demo's unqualified benchmark output.

## Backups, retention, and restore drills

There is no automated retention scheduler or off-host backup integration. The dashboard
`/api/backup` export is a per-account JSON export. It reads a key list and then reads
values individually; pause writes during export because it is not an atomic cluster
snapshot. Store exports encrypted off-host with access logging and an organization-approved
retention schedule. A candidate schedule for review is 7 daily, 4 weekly, and 12 monthly
copies; approve retention based on recovery needs, legal requirements, and data
sensitivity before use. Keep backup data separate from certificates, raw tokens, and
session secrets.

`/api/restore` merges the exported entries into the target namespace; it does not remove
keys absent from the export. For authenticated dashboards, sign in as the matching user;
cross-user restore is rejected. Before using a restore operationally:

1. Create a disposable cluster with the same build and compatible config.
2. Restore one export and compare expected keys/values and namespace isolation.
3. Verify that unrelated target keys remain, since restore is a merge.
4. Record elapsed time, operator steps, and any failed keys; repeat after relevant
   upgrades or configuration changes.
5. Keep the export until the drill is reviewed, then retain/destroy it according to the
   approved retention policy.

The raw `data_root` contains per-node Raft metadata, WALs, and snapshots. There is no
supported live cluster-wide filesystem backup/restore procedure. Do not copy one node's
Raft files into every node directory or restore stale node files into a running cluster.
If preserving the complete raw data root, stop all nodes cleanly first, copy the complete
root off-host, and treat recovery as an unverified disaster-recovery exercise on a
disposable cluster. Do not claim an RPO/RTO from this procedure.

## Disaster recovery, migration, and rollback

Until a recovery plan is qualified, the conservative data-loss boundary is the most
recent independently stored application export; no RPO is promised. If the cluster is
unavailable, stop automated restart loops, preserve all node data directories and logs,
record the last known leader/term and configuration, and recover only on an isolated
copy. Do not delete or rewrite original Raft state while diagnosing.

Voting membership is static. Do not change the peer set as a recovery or migration
shortcut. No schema migration, rolling binary upgrade, downgrade, or cross-version data
compatibility guarantee exists. For a version change, rehearse on a copy, export
application data, stop the entire cluster, preserve an off-host copy, install the same
new build/config on every node, and validate on disposable data before resuming writes.
Never roll back a binary against data written by a newer build; restore through a
separately rehearsed application-level recovery path.

## Reproducible build and deployment

Build from a reviewed source commit in a clean environment with the C++17 compiler,
CMake, gRPC, Protobuf, GoogleTest, OpenSSL, and Python dependencies documented in
[the testing guide](TESTING.md). Record the source commit, compiler and package versions,
build flags, generated Protobuf version, and artifact checksums with each deployment.
The repository does not yet publish signed release artifacts, a dependency lockfile,
or a supported container/package. Pin and review dependencies in the deployment's
approved build process; do not silently update a running cluster.

Create a local install tree or TGZ package from the configured build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=ON
cmake --build build --parallel
cmake --install build --prefix "$PWD/stage"
cmake --build build --target package
```

The package contains the server, C++ client, optional benchmark, credential rotation
helper, and example config;
it does not include credentials, a data root, or bundled operating-system libraries.

Keep config examples node-specific and protected. Verify file permissions, bind
addresses, TLS SANs, static peer membership, node token roles, data-root ownership,
firewall rules, metrics scrape paths, restart policy, and backup destinations before
starting the cluster. `--insecure-dev` and the `start.sh` demo are not deployment options.
