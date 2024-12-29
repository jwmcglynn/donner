import http.server
import socketserver
import argparse
import os

parser = argparse.ArgumentParser(description="Simple HTTP server")
parser.add_argument("--dir", "-d", default=".", help="directory to serve")
args = parser.parse_args()

os.chdir(args.dir)

class CrossOriginHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        return super().end_headers()

# Find the first free port in the range of 8000-8020
port = 8000
while port < 8020:
    try:
        socketserver.TCPServer(("", port), None)
        break
    except OSError:
        port += 1

socketserver.ThreadingTCPServer.allow_reuse_address = True
with socketserver.TCPServer(("", port), CrossOriginHandler) as httpd:
    print(f"Serving at http://127.0.0.1:{port}")
    print("Press Ctrl+C to exit")

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nKeyboard interrupt received, exiting.")
        httpd.server_close()
