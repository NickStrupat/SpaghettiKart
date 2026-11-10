#!/usr/bin/env python3
"""Serves the browser build (docs/WEB.md) on this computer.

    python3 tools/web-serve.py [folder] [--port 8064]

The game uses threads, which a browser only allows on a "cross-origin isolated" page: one served with the two
headers added below. Python's own http.server does not send them.
"""

import argparse
import functools
import http.server
import os
import socketserver


class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        # A rebuilt game is picked up on reload: the browser asks whether its copy is still current.
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()


Handler.extensions_map[".wasm"] = "application/wasm"


class Server(http.server.ThreadingHTTPServer):
    def server_bind(self):
        # HTTPServer also looks up this computer's network name here, which can take a long time and nothing uses.
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


def main():
    default_folder = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build-web", "web")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("folder", nargs="?", default=default_folder, help="folder holding index.html")
    parser.add_argument("--port", type=int, default=8064)
    parser.add_argument("--bind", default="127.0.0.1", help="address to listen on (0.0.0.0 for the local network)")
    arguments = parser.parse_args()

    folder = os.path.abspath(arguments.folder)
    if not os.path.isfile(os.path.join(folder, "index.html")):
        parser.error(f"no index.html in {folder}: build the game first (docs/WEB.md)")
    handler = functools.partial(Handler, directory=folder)
    with Server((arguments.bind, arguments.port), handler) as server:
        print(f"Serving {folder} at http://localhost:{arguments.port}/", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
