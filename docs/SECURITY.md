# Security Setup and Credential Operations

The supported security baseline for controlled experiments is mutual TLS plus a
role-scoped gRPC bearer token. Normal server, C++ client, and gateway startup refuses
plaintext or missing authorization credentials. `--insecure-dev` is an explicit
loopback-only exception used by `start.sh` and local tests; never use it for a remote
node or gateway.

This is not a production security certification. A server reloads token policy on
SIGHUP, and optional gRPC CRL directories are supported by the C++ server and clients.
The Python gateway's gRPC API does not expose CRL configuration: server-side CRLs still
reject revoked gateway/client certificates, but the gateway cannot independently reject
a revoked node certificate via CRL. Use CA rotation to remove trust in a revoked node
certificate from that gateway. The rotation helper stages and atomically activates
credential bundles, but an operator must verify every node and gateway after reload.
Per-key gRPC scopes are optional; tokens without an explicit scope retain the legacy
role-wide keyspace access. A global gRPC connection count and separately privileged
backup RPC are not implemented.

## Trust and role model

Every gRPC request must present a certificate signed by the configured CA and an
`authorization: Bearer <token>` metadata value. The server stores only the token's
lowercase SHA-256 digest in its authorization file. Generate tokens with at least 256
bits of entropy and keep raw tokens in separate mode-`0600` files.

| Role | RPCs |
|---|---|
| `read` | `Get`, `ListKeys` |
| `write` | `Set`, `Delete` |
| `monitor` | `GetClusterStatus` |
| `node` | Raft election, replication, snapshot, and heartbeat RPCs |

Use a node token with only `node,monitor`. A client should receive only the data and
monitor roles it needs. A gateway token needs `read,write,monitor` for dashboard CRUD,
backup/restore, and cluster status. Gateway account sessions further isolate dashboard
keys by username; do not expose the gateway's gRPC token to browser code.

An authorization-file line may include optional byte-prefix scopes:

```text
<lowercase-sha256>=read,write;read_prefixes=tenant/alice/;write_prefixes=tenant/alice/
```

`Get` and `ListKeys` enforce `read_prefixes`; `Set` and `Delete` enforce
`write_prefixes`. Prefixes are literal byte prefixes, not path globs. Use a delimiter
such as `/` to avoid accidental namespace overlap. A scope may contain comma-separated
prefixes; use `*` alone for unrestricted access. Omitting a scope preserves the older
role-wide behavior. Prefixes containing commas or semicolons, or leading/trailing
whitespace, cannot be represented in this file format. Node and monitor roles do not
accept key scopes.

Forwarded client RPCs preserve the original bearer token. A node token alone cannot
read or change keys. Node peers are still a trusted Raft security boundary: any
authorized node can participate in consensus and may affect availability.

## Provision credentials

Run these examples in a protected administrative shell. Keep the CA private key offline
or in an approved secrets manager; distribute only the CA certificate, signed workload
certificates, role-hash file, and each workload's own token file. Do not put credentials
or private keys in the repository, command-line arguments, logs, or backups.

Create a protected credential directory and a deployment CA:

```bash
umask 077
install -d -m 700 "$HOME/.config/raft-kv/pki" "$HOME/.config/raft-kv/auth"
cd "$HOME/.config/raft-kv/pki"
openssl req -x509 -newkey rsa:3072 -nodes -days 365 \
  -keyout ca-key.pem -out ca.pem -subj "/CN=Raft KV deployment CA"
chmod 600 ca-key.pem
chmod 644 ca.pem
```

Issue a certificate for every server name or IP address that clients use. Replace
`node-1.internal` and `10.0.0.11` with the node's actual DNS name and private address;
include `DNS:localhost,IP:127.0.0.1` only for local testing.

```bash
openssl req -new -newkey rsa:3072 -nodes \
  -keyout node-1-key.pem -out node-1.csr -subj "/CN=node-1.internal"
printf '%s\n' \
  'basicConstraints=critical,CA:FALSE' \
  'keyUsage=critical,digitalSignature,keyEncipherment' \
  'extendedKeyUsage=serverAuth,clientAuth' \
  'subjectAltName=DNS:node-1.internal,IP:10.0.0.11' > node-1.ext
openssl x509 -req -in node-1.csr -CA ca.pem -CAkey ca-key.pem \
  -CAcreateserial -days 90 -sha256 -extfile node-1.ext -out node-1.pem
chmod 600 node-1-key.pem
```

Issue a separate certificate and key for each node and each client workload. Use the
names clients actually dial in the certificate Subject Alternative Name (SAN); gRPC
verifies the server name. Protect and distribute private keys individually.

Generate and register a node token and a least-privilege client token:

```bash
cd "$HOME/.config/raft-kv/auth"
openssl rand -hex 32 > node.token
openssl rand -hex 32 > dashboard.token
node_hash="$(tr -d '\r\n' < node.token | sha256sum | cut -d' ' -f1)"
dashboard_hash="$(tr -d '\r\n' < dashboard.token | sha256sum | cut -d' ' -f1)"
printf '%s=node,monitor\n%s=read,write,monitor\n' \
  "$node_hash" "$dashboard_hash" > tokens.conf
chmod 600 node.token dashboard.token tokens.conf
```

Create a separate random token for each client or gateway. Add only its digest and
necessary role names to `tokens.conf`; deliver the raw token only to that workload.
The server rejects malformed role names, duplicate hashes, missing files, and files
that are accessible to group or other users.

## Configure and run

For each server, copy [the node config example](../config/node.conf.example) to a
private, node-specific file. Set a reachable private bind address, static peer list,
CA bundle, that node's certificate/key, the shared authorization file, and that
node's token file. Both `auth_file` and `token_file` are required together.
The default bind is `127.0.0.1`; set `bind` to a private interface address only when
peer nodes must reach it. Apply firewall rules so only cluster peers and approved
clients can reach the gRPC port.

```ini
bind = 10.0.0.11
port = 50051
peers = node-2.internal:50052, node-3.internal:50053
data_root = /var/lib/raft-kv
ca = /etc/raft-kv/pki/ca.pem
cert = /etc/raft-kv/pki/node-1.pem
key = /etc/raft-kv/pki/node-1-key.pem
auth_file = /etc/raft-kv/auth/tokens.conf
token_file = /etc/raft-kv/auth/node.token
```

Start with `build/server --config /etc/raft-kv/node.conf`. The gateway and C++ client
must also present a client certificate, key, CA, and private token file. For example:

```bash
python3 gateway.py --host 127.0.0.1 --target node-1.internal:50051 \
  --ca /etc/raft-kv/pki/ca.pem \
  --cert /etc/raft-kv/pki/dashboard-client.pem \
  --key /etc/raft-kv/pki/dashboard-client-key.pem \
  --token-file /etc/raft-kv/auth/dashboard.token \
  --users-file /etc/raft-kv/auth/dashboard-users.json \
  --https-cert /etc/raft-kv/pki/gateway.pem \
  --https-key /etc/raft-kv/pki/gateway-key.pem
```

For clients, use the same `--ca`, `--cert`, `--key`, and `--token-file` options.
Protect token, private-key, and user files with mode `0600`; protect their parent
directories from untrusted users. Configure an independent strong
`GATEWAY_SESSION_SECRET`. Bind a remotely reachable gateway only over HTTPS, with
dashboard authentication, explicit `--allow-public`, a private network/firewall, and
an approved dashboard origin. The local demo launcher intentionally uses
`--insecure-dev` on loopback and is not a production deployment template.

## Rotation, expiry, revocation, and incident response

- **Stage a credential bundle:** create a new sibling directory containing `ca.pem`,
  `cert.pem`, `key.pem`, `auth.conf`, `node.token`, and `crls/`. Keep the directory
  private (mode `0700`), and keep `key.pem`, `auth.conf`, and `node.token` at mode
  `0600` or stricter. Point each node's TLS, auth, token, and CRL config paths through
  one stable symlink such as `/etc/raft-kv/credentials/current/`. Use node-specific
  bundles where identities or node tokens differ.
- **Activate a staged node bundle:** after reviewing the new certificate SAN/EKU,
  token digests/roles, and CRLs, run the rotation helper once per host with every local
  server PID:

  ```bash
  rotate_credentials.py \
    --current-link /etc/raft-kv/credentials/current \
    --new-directory /etc/raft-kv/credentials/version-2026-11 \
    --pid "$(pgrep -x server)"
  ```

  For multiple nodes, execute on each host with its local server PID. The helper checks
  bundle permissions, atomically switches the symlink, and sends SIGHUP. The server
  reloads token roles/node tokens; its gRPC file watcher reloads CA/certificate/key
  files. Confirm server logs contain `security_credentials_reloaded` on every node and
  test the new credentials before removing old trust. A failed reload is logged and
  retains the previous in-memory token policy. The helper cannot prove successful
  authentication or roll back a partially applied multi-host rotation.
- **Routine token rotation:** keep both old and new token hashes valid during the
  overlap. Install the new token file for its intended workload, activate/reload the
  server policy, verify access and role boundaries, then remove the old digest and
  reload all servers again. The C++ client and gateway read the token file for each
  call; restart the gateway to reload its TLS certificate/key. Do not revoke the old
  token until every caller has switched.
- **Certificate rotation under the same CA:** issue replacement certificates before
  expiry, verify SAN/EKU, stage the matching key/certificate in the credential bundle,
  and switch/reload one node at a time. Monitor certificate expiration externally; the
  project has no expiry alert. The gateway must be restarted to load changed TLS files.
- **CA rotation:** distribute a CA bundle containing both old and new public CA
  certificates to every node/client and restart; issue/deploy new leaf certificates;
  verify all peers; then remove the old CA and restart again. Keep the old CA private
  key offline and destroy it according to the organization's key policy.
- **Certificate revocation:** CRLs are optional and configured with
  `crl_directory = /etc/raft-kv/credentials/current/crls` (or
  `--crl-directory`). The gRPC implementation requires an OpenSSL-backed build and
  hashed X.509 CRL files in the directory (prepare the CRL using the CA's protected
  issuance process, then run `openssl rehash <crl-directory>`). Revocation is not
  effective until every relevant server/client has loaded the updated CRL; restart
  those processes after changing CRL files. C++ clients and benchmarks can use
  `--crl-directory`; the Python gateway does not currently have a corresponding
  grpcio API option. Its mTLS client certificate is still checked against each node's
  CRL. To revoke a node server certificate for a gateway, rotate the trusted CA bundle.
  The CRL option is experimental in gRPC and is not a substitute for an independently
  reviewed PKI process. OCSP is not supported.
- **Token revocation:** remove the digest from the authorization file and send SIGHUP
  to every server, or activate a staged bundle with the rotation helper. Verify the
  revoked token fails on every node. If the CA private key is compromised, isolate
  affected processes, replace the CA, reissue all certificates, rotate every token,
  and restore service only after the new trust configuration is verified.
- **Emergency replacement:** isolate affected workloads and block their network access
  immediately. Preserve incident evidence without copying raw credentials into logs or
  tickets. For a leaked node token or CA key, stop affected nodes, replace credentials
  cluster-wide, restart from a known-good configuration, verify peer membership and
  quorum, then document approvals and the incident.
- **Approval and custody:** an authorized operator must approve each production
  credential, role assignment, host binding, and rotation. Use an organization-approved
  secrets manager or restricted deployment mechanism; do not use shell history,
  source control, shared logs, or dashboard backups as secret storage.

## Resource and threat boundaries

The gRPC server limits concurrent HTTP/2 streams to 64 per connection and accepted
inbound RPC messages to 16 MiB. The gateway caps request bodies at 10 MiB, limits login
attempts, and handles at most 64 HTTP worker threads; excess connections receive 503.
These controls bound individual connections and requests but do not cap total TCP
connections, process memory, aggregate stored data, request rate for authenticated
gRPC clients, or cluster-wide capacity. Enforce network connection/rate controls at
the host firewall or approved proxy and monitor the process externally.

The threat model assumes trusted node hosts and operators, private network controls,
protected at-rest files, and a CA whose signing key is not available to the cluster.
The implementation does not protect against a compromised node/host, stolen authorized
client credentials before revocation, malicious authorized writes, traffic analysis,
or denial of service from an unbounded number of TCP connections. Complete an
independent security/dependency review and credential approval before any production
use.

The optional node metrics listener is unauthenticated HTTP and defaults to loopback.
Keep it bound to loopback or restrict it to a trusted local scraper; never expose it
to the public internet.
