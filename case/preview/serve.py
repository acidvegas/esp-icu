#!/usr/bin/env python3
# ESP-ICU case preview server - acidvegas

import http.server
import os


HOST = '100.125.210.117' # tailscale
PORT = 20010
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Handler(http.server.SimpleHTTPRequestHandler):
	def end_headers(self):
		self.send_header('Cache-Control', 'no-store') # models change on every rebuild
		super().end_headers()


if __name__ == '__main__':
	os.chdir(ROOT)
	http.server.ThreadingHTTPServer((HOST, PORT), Handler).serve_forever()
