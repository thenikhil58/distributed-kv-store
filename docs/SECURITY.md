# Security Setup and Credential Operations

The supported security baseline for controlled experiments is mutual TLS plus a
role-scoped gRPC bearer token. Normal server, C++ client, and gateway startup refuses
plaintext or missing authorization credentials. `--insecure-dev` is an explicit
loopback-only exception used by `start.sh` and local tests; never use it for a remote
node or gateway.

This is not a production security certification. The gRPC token file is read at
startup; token revocation requires a restart. Certificate revocation lists, automated
rotation, per-key gRPC ACLs, a global gRPC connection count, and a separately privileged
backup RPC are not implemented. Direct `read`/`write` roles can access the whole keyspace.
Per-account key isolation exists only at the authenticated dashboard gateway.

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

- **Routine token rotation:** provision a new random token and add its digest to every
  server authorization file; install the new raw token only on its intended workload.
  Restart the cluster in a maintenance window so all nodes load the same authorization
  file, update clients/gateway, verify each role, then remove the old digest and restart
  again. Do not assume authorization-file edits take effect live.
- **Certificate rotation under the same CA:** issue replacement certificates before
  expiry, verify SAN and EKU, deploy the new key/certificate, restart each node/client
  in a controlled maintenance window, and verify mTLS connectivity before removing old
  files. Monitor certificate expiration externally; the project has no expiry alert.
- **CA rotation:** distribute a CA bundle containing both old and new public CA
  certificates to every node/client and restart; issue/deploy new leaf certificates;
  verify all peers; then remove the old CA and restart again. Keep the old CA private
  key offline and destroy it according to the organization's key policy.
- **Revocation:** there is no CRL or OCSP integration. Revoke a token by deleting its
  digest and restarting every server; revoke a certificate by removing trust in its CA
  and replacing the affected certificate. If the CA private key is compromised, stop
  the cluster, create a new CA, reissue all certificates, rotate every token, and restore
  service only after the new trust configuration is verified.
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
