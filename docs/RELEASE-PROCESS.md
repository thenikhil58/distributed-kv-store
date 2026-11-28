# Release and Validation Process

The project has no production support contract or qualified release profile. A passing
CI run is a repository change gate, not evidence that the store is safe for production
data. Use disposable or independently recoverable data throughout qualification.

## Versioning and compatibility

- Releases use immutable `vMAJOR.MINOR.PATCH` Git tags; pre-release candidates use
  `-rc.N` suffixes. The CMake project version and release tag must match.
- Every cluster member must run the same release build and static membership
  configuration. Rolling upgrades and downgrades are unsupported.
- There is no cross-version Raft metadata, WAL, snapshot, or application backup
  compatibility guarantee. Preserve a stopped copy of all node data before an upgrade;
  rehearse application-level export/restore on a disposable cluster.
- A release note must identify the source commit, compiler, CMake, Protobuf/gRPC and
  OpenSSL versions, build flags, generated Protobuf compiler version, changes to
  authorization/config formats, and known limitations.

## Required repository gates

The GitHub Actions workflow must pass on the release commit:

1. Clean configure and build of server, client, tests, and benchmark.
2. C++ integration/configuration tests, Python gateway/rotation tests, and Python
   bytecode compilation.
3. AddressSanitizer + UndefinedBehaviorSanitizer test job.
4. Python dependency vulnerability audit.
5. macOS configure/build/test (without claiming CRL support unless the selected gRPC
   TLS backend supports it).

Do not waive a failing gate silently. Record an approved, scoped exception and its
expiration in the release record. Dependency audit results cover declared Python
dependencies only; they are not a complete software supply-chain review.
ThreadSanitizer is not a blocking gate until the gRPC/Abseil dependencies can be
instrumented or their runtime reports can be distinguished from application races
without broad suppressions. The current uninstrumented run found and led to a fix for
an application race, but also emitted dependency-internal reports and is not clean.

## Required operator qualification before production consideration

These are external gates and are not established by the repository CI:

- Independent consensus, persistence, security, and dependency review.
- Measured workload/latency/capacity across approved cluster sizes and storage/network
  profiles, with reviewed limits and resource budgets.
- Partition, delay/reordering, disk-full/I/O-failure, crash/power-loss and stale-node
  recovery exercises on the target environment.
- Restore and node-replacement drills, reviewed retention, and approved RTO/RPO.
- Credential custody and revocation procedures; confirmation of CRL support in the
  exact gRPC/OpenSSL build.
- Staging soak, monitoring/alert validation, rollback rehearsal, and operator sign-off.

## Candidate release steps

1. Freeze the candidate commit and ensure the worktree is clean.
2. Pass all repository gates; archive CI logs and record the toolchain/dependency
   versions.
3. Build and checksum the install tree and TGZ package; inspect contents to ensure no
   credentials or runtime data are present.
4. Complete all operator qualification gates for the intended environment, or label
   the artifact explicitly as an experimental pre-release with those gates open.
5. Publish release notes, known limitations, source commit, checksums, and the exact
   compatibility statement. Do not claim signed provenance until signing and
   verification are implemented.
6. Create an immutable version tag only after maintainer approval. A tag alone does not
   imply production qualification.
