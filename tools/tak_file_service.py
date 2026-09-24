"""The file side of a TAK server, as much of it as ATAK uses to send a file.

ATAK sends a data package or a QuickPic to its streaming host's port 8443. On
the deck, nginx terminates TLS there for waydroid0 only and proxies to this, on
loopback (see *PR D2* in docs/TAKDeliveryPlan.md). Four calls, captured from
ATAK 5.8 on 2026-09-22:

    GET  /Marti/sync/missionquery?hash=H          200 + URL if held, else 404
    POST /Marti/sync/missionupload?hash=H&filename=F&creatorUid=U
    PUT  /Marti/api/sync/metadata/H/tool          tag; accepted and ignored
    GET  /Marti/sync/content?hash=H               the file, for a receiving ATAK

Everything else is 404. Nothing here is authenticated beyond where it listens:
loopback, reached only through the waydroid0 block.
"""

import email.parser
import email.policy
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

import tak_files

DEFAULT_PORT = 18443


def _upload_body(content_type, body):
    """The file inside an upload: the multipart part, or the body itself.

    ATAK posts multipart/form-data with the file in a part named `assetfile`;
    a raw body is accepted too. The hash check afterwards is what decides
    whether it is the file, so a guess here cannot store the wrong bytes.
    """
    if content_type and content_type.lower().startswith("multipart/"):
        head = b"Content-Type: " + content_type.encode("latin-1") + b"\r\n\r\n"
        message = email.parser.BytesParser(policy=email.policy.HTTP).parsebytes(head + body)
        parts = [part for part in message.iter_parts()]
        named = [part for part in parts
                 if part.get_param("name", header="content-disposition") == "assetfile"]
        chosen = (named or parts or [None])[0]
        return chosen.get_payload(decode=True) if chosen is not None else None
    return body


def content_disposition(name):
    """An attachment header for `name` that cannot become another header.

    The name is ATAK's, and came over the mesh in an offer: control
    characters (a CR/LF would start a new header), quotes and backslashes go,
    and a name with anything beyond ASCII is carried RFC 5987-encoded beside
    a plain fallback.
    """
    from urllib.parse import quote
    safe = "".join(c for c in str(name) if c.isprintable() and c not in '"\\') or "file"
    plain = safe.encode("ascii", "replace").decode("ascii")
    header = 'attachment; filename="%s"' % plain
    if plain != safe:
        header += "; filename*=UTF-8''%s" % quote(safe, safe="")
    return header


class FileService:
    """ATAK's upload and download, backed by a FileStore."""

    def __init__(self, store, public_base, port=DEFAULT_PORT, host="127.0.0.1",
                 on_upload=None, log=print):
        self.store = store
        self.public_base = public_base.rstrip("/")
        self.on_upload = on_upload
        self.log = log
        service = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, fmt, *args):   # the bridge log is enough
                pass

            def _reply(self, code, body=b"", content_type="text/plain", extra=None):
                self.send_response(code)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(body)))
                for key, value in (extra or {}).items():
                    self.send_header(key, value)
                self.end_headers()
                if body:
                    self.wfile.write(body)

            def _query(self):
                split = urlsplit(self.path)
                return split.path, {k: v[0] for k, v in parse_qs(split.query).items()}

            def do_GET(self):
                path, query = self._query()
                file_hash = (query.get("hash") or "").lower()
                if path == "/Marti/sync/missionquery":
                    if service.store.has(file_hash):
                        return self._reply(200, service.url_for(file_hash).encode())
                    return self._reply(404, b"not found")
                if path == "/Marti/sync/content":
                    data = service.store.read(file_hash)
                    if data is None:
                        service.log("[files] ATAK asked for %s, which is not held" % file_hash[:16])
                        return self._reply(404, b"not found")
                    name = service.store.meta(file_hash).get("filename") or file_hash
                    service.log("[files] served %s (%d bytes) to ATAK" % (name, len(data)))
                    return self._reply(200, data, "application/octet-stream",
                                       {"Content-Disposition": content_disposition(name)})
                service.log("[files] ATAK asked for %s; not something this serves" % path)
                return self._reply(404, b"not found")

            def do_POST(self):
                path, query = self._query()
                if path != "/Marti/sync/missionupload":
                    return self._reply(404, b"not found")
                length = int(self.headers.get("Content-Length") or 0)
                if length > tak_files.MAX_FILE_BYTES * 2:
                    return self._reply(413, b"too large")
                body = self.rfile.read(length)
                data = _upload_body(self.headers.get("Content-Type"), body)
                expected = (query.get("hash") or "").lower() or None
                stored = service.store.put(
                    data or b"", filename=query.get("filename", ""), expected_hash=expected,
                    creator_uid=query.get("creatorUid"))
                if stored is None:
                    service.log("[files] upload refused: it is not the file its hash names")
                    return self._reply(400, b"hash mismatch")
                service.log("[files] ATAK uploaded %s (%d bytes, %s)"
                            % (query.get("filename", "?"), len(data), stored[:16]))
                if service.on_upload:
                    service.on_upload(stored)
                return self._reply(200, service.url_for(stored).encode())

            def do_PUT(self):
                path, _ = self._query()
                length = int(self.headers.get("Content-Length") or 0)
                if length:
                    self.rfile.read(length)
                if path.startswith("/Marti/api/sync/metadata/"):
                    return self._reply(200)
                return self._reply(404, b"not found")

        self.server = ThreadingHTTPServer((host, port), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]

    def url_for(self, file_hash):
        return tak_files.content_url(self.public_base, file_hash)

    def start(self):
        thread = threading.Thread(target=self.server.serve_forever, daemon=True,
                                  name="tak-file-service")
        thread.start()
        return thread

    def stop(self):
        self.server.shutdown()
        self.server.server_close()
