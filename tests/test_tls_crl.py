import hashlib
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest

import grpc

ROOT = Path(__file__).resolve().parents[1]


def run_openssl(*arguments):
    subprocess.run(["openssl", *arguments], check=True, capture_output=True, text=True)


class TlsCrlTests(unittest.TestCase):
    def test_server_rejects_revoked_client_certificate(self):
        binary = Path(os.environ.get("KVSTORE_SERVER_BINARY", ROOT / "build" / "server"))
        if not binary.is_file():
            self.skipTest(f"server binary not found: {binary}")

        with tempfile.TemporaryDirectory(prefix="kvstore-crl-") as directory:
            root = Path(directory)
            os.chmod(root, 0o700)
            new_certs = root / "newcerts"
            crls = root / "crls"
            new_certs.mkdir(mode=0o700)
            crls.mkdir(mode=0o700)
            for name in ("index.txt",):
                (root / name).write_text("", encoding="ascii")
            (root / "serial").write_text("1000\n", encoding="ascii")
            (root / "crlnumber").write_text("1000\n", encoding="ascii")

            ca_key = root / "ca.key"
            ca_cert = root / "ca.pem"
            run_openssl(
                "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
                "-keyout", str(ca_key), "-out", str(ca_cert), "-subj", "/CN=KVStore test CA",
                "-addext", "basicConstraints=critical,CA:TRUE",
                "-addext", "keyUsage=critical,keyCertSign,cRLSign",
            )
            os.chmod(ca_key, 0o600)

            ca_config = root / "ca.cnf"
            ca_config.write_text(
                "[ca]\n"
                "default_ca=default\n"
                "[default]\n"
                f"database={root / 'index.txt'}\n"
                f"serial={root / 'serial'}\n"
                f"crlnumber={root / 'crlnumber'}\n"
                f"new_certs_dir={new_certs}\n"
                f"certificate={ca_cert}\n"
                f"private_key={ca_key}\n"
                "default_md=sha256\n"
                "default_days=2\n"
                "default_crl_days=2\n"
                "policy=policy\n"
                "copy_extensions=copy\n"
                "[policy]\n"
                "commonName=supplied\n"
                "[server_cert]\n"
                "basicConstraints=critical,CA:FALSE\n"
                "keyUsage=critical,digitalSignature,keyEncipherment\n"
                "extendedKeyUsage=serverAuth,clientAuth\n"
                "subjectAltName=DNS:localhost,IP:127.0.0.1\n"
                "[client_cert]\n"
                "basicConstraints=critical,CA:FALSE\n"
                "keyUsage=critical,digitalSignature,keyEncipherment\n"
                "extendedKeyUsage=clientAuth\n"
                "[crl_ext]\n"
                "authorityKeyIdentifier=keyid:always\n",
                encoding="ascii",
            )

            def issue_certificate(name, common_name, extension):
                key = root / f"{name}.key"
                csr = root / f"{name}.csr"
                cert = root / f"{name}.pem"
                run_openssl(
                    "req", "-new", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", str(key), "-out", str(csr),
                    "-subj", f"/CN={common_name}",
                )
                os.chmod(key, 0o600)
                run_openssl(
                    "ca", "-batch", "-config", str(ca_config), "-extensions", extension,
                    "-in", str(csr), "-out", str(cert), "-notext",
                )
                return key, cert

            server_key, server_cert = issue_certificate("server", "localhost", "server_cert")
            valid_key, valid_cert = issue_certificate("valid-client", "valid-client", "client_cert")
            revoked_key, revoked_cert = issue_certificate("revoked-client", "revoked-client", "client_cert")
            run_openssl("ca", "-batch", "-config", str(ca_config), "-revoke", str(revoked_cert))
            crl = crls / "revoked.pem"
            run_openssl(
                "ca", "-batch", "-config", str(ca_config), "-gencrl",
                "-crlexts", "crl_ext", "-out", str(crl),
            )
            run_openssl("rehash", str(crls))

            token = "test-monitor-token-0123456789-abcdefghijkl"
            token_file = root / "node.token"
            token_file.write_text(token + "\n", encoding="ascii")
            os.chmod(token_file, 0o600)
            auth_file = root / "auth.conf"
            auth_file.write_text(
                hashlib.sha256(token.encode("ascii")).hexdigest() + "=monitor,node\n",
                encoding="ascii",
            )
            os.chmod(auth_file, 0o600)

            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                port = listener.getsockname()[1]
            process = subprocess.Popen(
                [
                    str(binary), str(port), "--bind", "127.0.0.1",
                    "--ca", str(ca_cert), "--cert", str(server_cert),
                    "--key", str(server_key), "--auth-file", str(auth_file),
                    "--token-file", str(token_file), "--crl-directory", str(crls),
                    "--data-root", str(root / "data"),
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            channels = []
            try:
                try:
                    import gateway
                    pb2, pb2_grpc = gateway.load_generated_client()
                except Exception as error:
                    self.fail(f"failed to load generated gRPC client: {error}")

                def create_stub(key, cert):
                    credentials = grpc.ssl_channel_credentials(
                        root_certificates=ca_cert.read_bytes(),
                        private_key=key.read_bytes(),
                        certificate_chain=cert.read_bytes(),
                    )
                    call_credentials = grpc.access_token_call_credentials(token)
                    channel_credentials = grpc.composite_channel_credentials(
                        credentials, call_credentials
                    )
                    channel = grpc.secure_channel(f"localhost:{port}", channel_credentials)
                    channels.append(channel)
                    return pb2_grpc.KVStoreStub(channel)

                valid_stub = create_stub(valid_key, valid_cert)
                deadline = time.monotonic() + 20
                while True:
                    try:
                        valid_stub.GetClusterStatus(pb2.ClusterStatusRequest(), timeout=2)
                        break
                    except grpc.RpcError:
                        if time.monotonic() >= deadline:
                            self.fail("server did not accept a valid client certificate")
                        time.sleep(0.1)

                revoked_stub = create_stub(revoked_key, revoked_cert)
                with self.assertRaises(grpc.RpcError):
                    revoked_stub.GetClusterStatus(pb2.ClusterStatusRequest(), timeout=5)
            finally:
                for channel in channels:
                    channel.close()
                if process.poll() is None:
                    process.terminate()
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
