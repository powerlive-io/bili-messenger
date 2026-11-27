"""Loopback-only integration test; never contacts PowerLive or submits real feedback."""
import email.parser
import email.policy
import faulthandler
import http.server
import socketserver
import subprocess
import sys
import threading


# HTTPServer reverse-resolves even 127.0.0.1 in server_bind(). The handler
# needs only a listening socket, not a hostname or a working DNS resolver.
class Server(socketserver.TCPServer):
    failures = []
    requests = 0


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        if self.path == "/feedback/prepare":
            # A redirect is an HTTP response, not an invitation to send the
            # request body (or credentials) to a second destination.
            try:
                assert self.headers["Cookie"] is None
                assert self.headers["Content-Type"] == "application/json"
                assert self.headers["Accept"] == "application/json"
                assert self.rfile.read(int(self.headers["Content-Length"])) == b'{"test":true}'
            except Exception as error:
                self.server.failures.append(repr(error))
            self.send_response(307)
            self.send_header("Location", "/unexpected")
            self.send_header("Set-Cookie", "session=test; Path=/")
            self.end_headers()
            self.wfile.write(b"not followed")
            return
        try:
            assert self.path == "/feedback"
            assert self.headers["Cookie"] is None
            assert self.headers["Authorization"] is None
            assert self.headers["X-Feedback-Token"] == "local-test-token"
            content_type = self.headers["Content-Type"]
            assert content_type.startswith("multipart/form-data; boundary=")
            length = int(self.headers["Content-Length"])
            assert 0 < length < 2048
            data = self.rfile.read(length)
            message = email.parser.BytesParser(policy=email.policy.default).parsebytes(
                f"Content-Type: {content_type}\r\n\r\n".encode() + data
            )
            parts = list(message.iter_parts())
            expected = {"message": "中文 @foo; 问题\n第二行 😀"}
            if self.server.requests & 1:
                expected["email"] = "test@example.com"
            if self.server.requests & 2:
                expected["log"] = "UTF-8 诊断日志\nop=message_send_or_recall http=200 api=-101\n"
            assert len(parts) == len(expected)
            actual = {}
            for part in parts:
                name = part.get_param("name", header="Content-Disposition")
                if name == "log":
                    assert part.get_filename() == "diagnostics.log"
                    assert part.get_content_type() == "text/plain"
                    assert part.get_content_charset() == "utf-8"
                else:
                    assert part.get_filename() is None
                actual[name] = part.get_payload(decode=True).decode("utf-8")
            assert actual == expected, (actual, expected)
            self.server.requests += 1
        except Exception as error:
            self.server.failures.append(repr(error))
        self.send_response(429)
        self.send_header("rEtRy-AfTeR", "600")
        self.send_header("Content-Length", "0")
        self.end_headers()


faulthandler.dump_traceback_later(10)  # Capture a stall before CTest's 20s deadline.
with Server(("127.0.0.1", 0), Handler) as server:
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        subprocess.run(
            [sys.argv[1], "--http", f"http://127.0.0.1:{server.server_address[1]}/feedback"],
            check=True,
            timeout=15,
        )
        assert not server.failures, server.failures
        assert server.requests == 4, server.requests
    finally:
        server.shutdown()
        thread.join()
faulthandler.cancel_dump_traceback_later()
