import http.server
import socketserver
import argparse
import os

parser = argparse.ArgumentParser(description="Simple HTTP server")
parser.add_argument("--dir", "-d", default=".", help="directory to serve")
args = parser.parse_args()

PORT = 8000

os.chdir(args.dir)

Handler = http.server.SimpleHTTPRequestHandler

with socketserver.TCPServer(("", PORT), Handler) as httpd:
  print(f"Serving at http://127.0.0.1:{PORT}")
  print("Press Ctrl+C to exit")

  try:
      httpd.serve_forever()
  except KeyboardInterrupt:
      print("\nKeyboard interrupt received, exiting.")
      httpd.server_close()
