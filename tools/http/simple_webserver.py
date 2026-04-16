import http.server
import socketserver
import argparse
import functools
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import threading

parser = argparse.ArgumentParser(description="Simple HTTP server")
parser.add_argument("--dir", "-d", default=".", help="directory to serve")
parser.add_argument(
    "--host",
    default="0.0.0.0",
    help="host/interface to bind (default: all IPv4 interfaces)",
)
parser.add_argument(
    "--https",
    action="store_true",
    help="serve over HTTPS using a local development certificate",
)
parser.add_argument("--certfile", help="path to TLS certificate PEM file")
parser.add_argument("--keyfile", help="path to TLS private key PEM file")
parser.add_argument(
    "--cert-dir",
    default=os.path.join(os.path.expanduser("~"), ".cache", "donner", "dev-certs"),
    help="directory for auto-generated local certificates",
)
args = parser.parse_args()
serve_dir = os.path.abspath(args.dir)


class CrossOriginHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        return super().end_headers()


def host_for_bind(bind_host: str) -> str:
    if bind_host in ("0.0.0.0", ""):
        return ""
    return bind_host


def find_free_port(bind_host: str) -> int:
    return find_free_port_excluding(bind_host, set())


def find_free_port_excluding(bind_host: str, excluded: set[int]) -> int:
    for port in range(8000, 8020):
        if port in excluded:
            continue
        try:
            probe = socketserver.TCPServer((bind_host, port), None)
            probe.server_close()
            return port
        except OSError:
            continue
    raise RuntimeError("No free port found in range 8000-8019")


def detect_lan_ip() -> str | None:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("8.8.8.8", 80))
            address = sock.getsockname()[0]
            if address and not address.startswith("127."):
                return address
    except OSError:
        pass

    try:
        address = socket.gethostbyname(socket.gethostname())
        if address and not address.startswith("127."):
            return address
    except OSError:
        pass

    return None


def ensure_local_certificate(cert_dir: Path, lan_ip: str | None) -> tuple[Path, Path]:
    cert_dir.mkdir(parents=True, exist_ok=True)
    cert_path = cert_dir / "server-cert.pem"
    key_path = cert_dir / "server-key.pem"
    meta_path = cert_dir / "server-meta.json"

    sans = {
        "dns": ["localhost"],
        "ip": ["127.0.0.1"],
    }
    if lan_ip:
        sans["ip"].append(lan_ip)

    existing_meta = None
    if meta_path.exists():
        try:
            existing_meta = json.loads(meta_path.read_text())
        except (OSError, json.JSONDecodeError):
            existing_meta = None

    if (
        cert_path.exists()
        and key_path.exists()
        and existing_meta == sans
    ):
        return cert_path, key_path

    config_lines = [
        "[req]",
        "distinguished_name = dn",
        "x509_extensions = v3_req",
        "prompt = no",
        "",
        "[dn]",
        "CN = localhost",
        "",
        "[v3_req]",
        "subjectAltName = @alt_names",
        "",
        "[alt_names]",
    ]

    dns_index = 1
    ip_index = 1
    for dns_name in sans["dns"]:
        config_lines.append(f"DNS.{dns_index} = {dns_name}")
        dns_index += 1
    for ip_address in sans["ip"]:
        config_lines.append(f"IP.{ip_index} = {ip_address}")
        ip_index += 1

    with tempfile.NamedTemporaryFile("w", delete=False) as config_file:
        config_file.write("\n".join(config_lines))
        config_path = Path(config_file.name)

    try:
        subprocess.run(
            [
                "openssl",
                "req",
                "-x509",
                "-nodes",
                "-newkey",
                "rsa:2048",
                "-sha256",
                "-days",
                "30",
                "-keyout",
                str(key_path),
                "-out",
                str(cert_path),
                "-config",
                str(config_path),
                "-extensions",
                "v3_req",
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    finally:
        config_path.unlink(missing_ok=True)

    meta_path.write_text(json.dumps(sans, indent=2, sort_keys=True))
    return cert_path, key_path


bind_host = host_for_bind(args.host)
lan_ip = detect_lan_ip() if bind_host == "" else None

certfile = None
keyfile = None
if args.certfile or args.keyfile:
    if not (args.certfile and args.keyfile):
        raise RuntimeError("--certfile and --keyfile must be provided together")
    certfile = Path(args.certfile)
    keyfile = Path(args.keyfile)
elif args.https:
    certfile, keyfile = ensure_local_certificate(Path(args.cert_dir), lan_ip)

socketserver.ThreadingTCPServer.allow_reuse_address = True
handler = functools.partial(CrossOriginHandler, directory=serve_dir)

if args.https:
    http_bind_host = "127.0.0.1"
    http_port = find_free_port(http_bind_host)
    https_port = find_free_port_excluding(bind_host, {http_port})

    localhost_url = f"http://127.0.0.1:{http_port}"
    lan_url = f"https://{lan_ip}:{https_port}" if lan_ip else None

    http_server = socketserver.ThreadingTCPServer((http_bind_host, http_port), handler)
    https_server = socketserver.ThreadingTCPServer((bind_host, https_port), handler)

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certfile=str(certfile), keyfile=str(keyfile))
    https_server.socket = context.wrap_socket(https_server.socket, server_side=True)

    threads = [
        threading.Thread(target=http_server.serve_forever, daemon=True),
        threading.Thread(target=https_server.serve_forever, daemon=True),
    ]
    for thread in threads:
        thread.start()

    print(f"Local URL: {localhost_url}")
    if lan_url:
        print(f"LAN URL:   {lan_url}")
    else:
        print(f"HTTPS URL: https://127.0.0.1:{https_port}")
    print(f"Cert file: {certfile}")
    print(f"Key file:  {keyfile}")
    print("Press Ctrl+C to exit")

    try:
        for thread in threads:
            thread.join()
    except KeyboardInterrupt:
        print("\nKeyboard interrupt received, exiting.")
        http_server.shutdown()
        https_server.shutdown()
        http_server.server_close()
        https_server.server_close()
else:
    port = find_free_port(bind_host)
    localhost_url = f"http://127.0.0.1:{port}"
    lan_url = f"http://{lan_ip}:{port}" if lan_ip else None

    with socketserver.ThreadingTCPServer((bind_host, port), handler) as httpd:
        print(f"Serving at {localhost_url}")
        if lan_url:
            print(f"LAN URL:   {lan_url}")
        print("Press Ctrl+C to exit")

        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nKeyboard interrupt received, exiting.")
            httpd.server_close()
