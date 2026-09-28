#!/usr/bin/env python3
# hub.py — hub di sync OHNX (PROTOTIPO LOCALE, resta in ohnx-hub/, fuori git).
# stdlib-only: gira ovunque, anche senza docker.
#
# Uso:  python3 hub.py [DATADIR] [--port 8321] [--compat-port 80]
#       OHNX_PASSWORD=xxx python3 hub.py   (default: "ohnx")
#
# API propria (v1, per Switch patchato / app iOS / futuri client):
#   GET  /ohnx                        -> {"service":"ohnx-sync","v":1,...}
#   POST /ohnx/login {"password":..}  -> {"token":..}
#   GET  /ohnx/list?path=<rel>        (X-Auth) -> {"isDir":..,"items":[{name,isDir,size,modified}]}
#   GET  /ohnx/raw?path=<rel>         (X-Auth) -> bytes
#   POST /ohnx/upload?path=<rel>      (X-Auth, body=bytes) -> {"ok":true,"size":n}
#
# Shim filebrowser (per le build Switch ATTUALI, senza patch):
#   GET  /                            -> pagina con "window.FileBrowser"
#   POST /api/login {"username","password","recaptcha":""} -> token (testo grezzo)
#   GET  /api/resources/<path>/       (X-Auth) -> stesso JSON di /ohnx/list
#   GET  /api/raw/<path>              (X-Auth) -> bytes
#   POST /api/resources/<path>?override=true (X-Auth) -> scrittura
import argparse, datetime, json, os, secrets, sys, time, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PASSWORD = os.environ.get("OHNX_PASSWORD", "ark")
TOKENS = {}  # token -> unix expiry
SHA_CACHE = {}  # abspath -> (mtime_ns, size, hex): evita riletture
SAVE_EXTS = (".sav", ".srm", ".dsv")


def file_sha(path):
    # SHA256 con cache su (mtime, size): il contenuto cambia solo se
    # cambiano questi. Solo per i save (le ROM da 16MB non si hashano mai).
    try:
        st = os.stat(path)
    except OSError:
        return ""
    key = (path, st.st_mtime_ns, st.st_size)
    hit = SHA_CACHE.get(path)
    if hit and hit[0] == key:
        return hit[1]
    try:
        import hashlib
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(65536), b""):
                h.update(chunk)
        hexd = h.hexdigest()
    except OSError:
        return ""
    if len(SHA_CACHE) > 2000:  # bound: lancio lungo, tanti file
        SHA_CACHE.clear()
    SHA_CACHE[path] = (key, hexd)
    return hexd


def new_token():
    t = secrets.token_hex(16)
    TOKENS[t] = time.time() + 2 * 3600
    return t


def token_ok(t):
    return t and TOKENS.get(t, 0) > time.time()


class Hub(BaseHTTPRequestHandler):
    root = "data"
    dist = ""  # --dist: fuori root, mai traversal (vedi _safe_dist)
    server_version = "ohnx-hub/0.1"

    def log_message(self, *a):
        sys.stderr.write("%s %s\n" % (self.address_string(), a[0] % a[1:]))

    # ---- helpers ----
    def _send_json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_text(self, text, code=200, ctype="text/plain"):
        body = text.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n > 0 else b""

    def _safe(self, rel):
        # Rifiuta traversal: il path finale deve restare dentro root.
        p = os.path.normpath(os.path.join(self.root, rel.lstrip("/")))
        if p != self.root and not p.startswith(self.root + os.sep):
            return None
        return p

    def _listing(self, rel):
        p = self._safe(rel)
        if p is None or not os.path.isdir(p):
            return None
        items = []
        for name in sorted(os.listdir(p)):
            fp = os.path.join(p, name)
            try:
                st = os.stat(fp)
            except OSError:
                continue
            isdir = os.path.isdir(fp)
            m = datetime.datetime.fromtimestamp(st.st_mtime, datetime.timezone.utc)
            entry = {"name": name, "isDir": isdir, "size": 0 if isdir else st.st_size,
                     "modified": m.strftime("%Y-%m-%dT%H:%M:%SZ")}
            if not isdir and name.lower().endswith(SAVE_EXTS):
                entry["sha"] = file_sha(fp)
            items.append(entry)
        return {"isDir": True, "items": items}

    def _need_auth(self):
        if token_ok(self.headers.get("X-Auth")):
            return True
        self._send_json({"error": "unauthorized"}, 401)
        return False

    # ---- GET ----
    def _safe_dist(self, rel):
        if not self.dist:
            return None
        p = os.path.normpath(os.path.join(self.dist, rel.lstrip("/")))
        if p != self.dist and not p.startswith(self.dist + os.sep):
            return None
        return p

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        path = u.path
        if path == "/ohnx":
            return self._send_json({"service": "ohnx-sync", "v": 1, "name": "ohnx-hub-dev"})
        if path == "/" :
            # Shim filebrowser: fingerprint atteso dallo scan Switch.
            return self._send_text(
                '<!doctype html><html><head><script>window.FileBrowser = '
                '{"ohnxHub":true};</script></head><body>ohnx hub</body></html>',
                ctype="text/html")
        # File di dist alla radice (niente /dist/ da digitare): latest.json,
        # latest-r36s.json, .nro/.zip per l'updater (update.cfg = http://host:porta/).
        # Solo file top-level, mai dir: il resto resta 404. Dopo /ohnx /api
        # /dist /upload, prima del fingerprint /.
        if path not in ("/ohnx", "/") and not path.startswith(("/ohnx/", "/api/",
                                                               "/dist/", "/upload")):
            rel = urllib.parse.unquote(path.lstrip("/"))
            if rel and "/" not in rel:
                p = self._safe_dist(rel)
                if p is not None and os.path.isfile(p):
                    try:
                        with open(p, "rb") as f:
                            data = f.read()
                    except OSError:
                        return self._send_text("read error", 500)
                    ctype = "application/json" if p.endswith(".json") else \
                        "application/octet-stream"
                    self.send_response(200)
                    self.send_header("Content-Type", ctype)
                    self.send_header("Content-Length", str(len(data)))
                    self.send_header("Cache-Control", "no-store")
                    self.end_headers()
                    try:
                        self.wfile.write(data)
                    except (BrokenPipeError, ConnectionResetError):
                        pass
                    return
            return self._send_text("not found", 404)
        if path == "/dist/" or path.startswith("/dist/"):
            # Server file di dist (update Switch/R36S + latest.json).
            p = self._safe_dist(urllib.parse.unquote(path[len("/dist/"):]))
            if p is None or not os.path.isfile(p):
                return self._send_text("not found", 404)
            try:
                with open(p, "rb") as f:
                    data = f.read()
            except OSError:
                return self._send_text("read error", 500)
            ctype = "application/json" if p.endswith(".json") else \
                "application/octet-stream"
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        if path == "/ohnx/list" or path.startswith("/api/resources/"):
            if not self._need_auth():
                return
            rel = q.get("path", [""])[0] if path == "/ohnx/list" else \
                urllib.parse.unquote(path[len("/api/resources/"):])
            if rel.endswith("/") :
                rel = rel[:-1]
            out = self._listing(rel)
            if out is None:
                return self._send_json({"error": "not found"}, 404)
            return self._send_json(out)
        if path == "/ohnx/raw" or path.startswith("/api/raw/"):
            if not self._need_auth():
                return
            rel = q.get("path", [""])[0] if path == "/ohnx/raw" else \
                urllib.parse.unquote(path[len("/api/raw/"):])
            p = self._safe(rel)
            if p is None or not os.path.isfile(p):
                return self._send_text("not found", 404)
            try:
                with open(p, "rb") as f:
                    data = f.read()
            except OSError:
                return self._send_text("read error", 500)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                # Client che chiude a meta' (timeout/cancel Switch sui file
                # grossi): normale, una riga e via, niente traceback.
                sys.stderr.write("abort client su %s\n" % rel)
            return
        return self._send_text("not found", 404)

    # ---- POST ----
    def _store_upload(self, subdir, ext, cap):
        # POST /upload /upload-save come serve_upload.py: ?f=tag, file
        # <tag>_<ts>_<client><ext> in dist/uploads/<subdir>/.
        if not self.dist:
            return self._send_text("dist non abilitata (--dist)", 404)
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        tag = "".join(q.get("f", ["debug"]))[:24] or "debug"
        tag = "".join(c if (c.isalnum() or c in "-_") else "_" for c in tag)
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0 or n > cap:
            return self._send_text("body vuoto o troppo grande", 400)
        try:
            data = self.rfile.read(n)
        except OSError:
            return self._send_text("lettura fallita", 500)
        stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S_%f")
        client = self.client_address[0].replace(".", "_")
        out = os.path.join(self.dist, "uploads", subdir,
                           "%s_%s_%s%s" % (tag, stamp, client, ext))
        try:
            with open(out, "wb") as f:
                f.write(data)
        except OSError as e:
            sys.stderr.write("upload %s: %s\n" % (out, e))
            return self._send_text("scrittura fallita", 500)
        sys.stderr.write("[upload] %d byte -> %s\n" % (len(data), out))
        return self._send_text("ok")

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        path = u.path
        if path == "/upload":
            return self._store_upload("logs", ".log", 5 * 1024 * 1024)
        if path == "/upload-save":
            return self._store_upload("saves", ".sav", 128 * 1024 * 1024)
        raw = self._body()
        if path == "/ohnx/login":
            try:
                cred = json.loads(raw.decode() or "{}")
            except ValueError:
                cred = {}
            if cred.get("password") == PASSWORD:
                return self._send_json({"token": new_token()})
            return self._send_json({"error": "unauthorized"}, 401)
        if path == "/api/login":
            try:
                cred = json.loads(raw.decode() or "{}")
            except ValueError:
                cred = {}
            # Dialetto filebrowser classico: token come stringa grezza.
            if cred.get("password") == PASSWORD:
                return self._send_text(new_token())
            return self._send_text("unauthorized", 401)
        if path == "/ohnx/upload" or path.startswith("/api/resources/"):
            if not self._need_auth():
                return
            rel = q.get("path", [""])[0] if path == "/ohnx/upload" else \
                urllib.parse.unquote(path[len("/api/resources/"):])
            p = self._safe(rel)
            if p is None:
                return self._send_json({"error": "bad path"}, 400)
            try:
                parent = os.path.dirname(p)
                if parent:
                    os.makedirs(parent, exist_ok=True)
                if os.path.isfile(p):
                    # Rete di sicurezza: la versione sovrascritta resta in
                    # <nome>.bak.<timestamp> (mai persa in automatico).
                    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
                    try:
                        os.replace(p, p + ".bak." + stamp)
                    except OSError:
                        pass
                with open(p, "wb") as f:
                    f.write(raw)
            except OSError as e:
                sys.stderr.write("upload %s: %s\n" % (p, e))
                return self._send_json({"error": "write error"}, 500)
            return self._send_json({"ok": True, "size": len(raw)})
        return self._send_text("not found", 404)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("datadir", nargs="?", default="data")
    ap.add_argument("--dist", default="",
                    help="cartella dist da servire (update+log): abilita GET"
                    " /dist/<file> e POST /upload /upload-save come"
                    " serve_upload.py (stessi path e nomi file)")
    ap.add_argument("--port", type=int, default=8321)
    ap.add_argument("--compat-port", type=int, default=0,
                    help="seconda porta con TUTTE le API (comodo per debug via browser)."
                    " La Switch parla il dialetto filebrowser sulla porta API"
                    " principale, niente piu' porta 80 ne' sudo (0=spento)")
    a = ap.parse_args()
    os.makedirs(a.datadir, exist_ok=True)
    Hub.root = os.path.abspath(a.datadir)
    Hub.dist = os.path.abspath(a.dist) if a.dist else ""
    if Hub.dist:
        os.makedirs(os.path.join(Hub.dist, "uploads", "logs"), exist_ok=True)
        os.makedirs(os.path.join(Hub.dist, "uploads", "saves"), exist_ok=True)
        print("ohnx hub: dist %s (update+log)" % Hub.dist, flush=True)
    # uuid persistente: identita' stabile oltre hostname/DHCP (vedi pairing Switch).
    uuidFile = os.path.join(Hub.root, ".ohnx_uuid")
    try:
        with open(uuidFile) as f:
            hubUuid = f.read().strip()
    except OSError:
        hubUuid = ""
    if not hubUuid:
        import uuid as _uuid
        hubUuid = str(_uuid.uuid4())
        try:
            with open(uuidFile, "w") as f:
                f.write(hubUuid)
        except OSError:
            pass
    srv = ThreadingHTTPServer(("0.0.0.0", a.port), Hub)
    print("ohnx hub: root=%s API=:8321->%d" % (Hub.root, a.port), flush=True)
    if a.compat_port:
        try:
            compat = ThreadingHTTPServer(("0.0.0.0", a.compat_port), Hub)
            import threading
            threading.Thread(target=compat.serve_forever, daemon=True).start()
            print("ohnx hub: shim filebrowser su porta %d" % a.compat_port, flush=True)
        except (OSError, PermissionError) as e:
            print("ohnx hub: porta %d non avviata (%s)" % (a.compat_port, e), flush=True)
    # Publish mDNS _ohnx-sync._tcp (opzionale: richiede zeroconf).
    # TXT: v=versione contratto, api=path API propria, fb=1 se lo shim
    # filebrowser (porta compat) e' attivo. Niente segreti nel TXT.
    zc = None
    try:
        from zeroconf import ServiceInfo, Zeroconf
        import socket

        def _lan_ip():
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                s.connect(("192.168.4.1", 80))  # nessun traffico, solo scelta interfaccia
                ip = s.getsockname()[0]
                s.close()
                return ip
            except OSError:
                return None

        lip = _lan_ip()
        svc = ServiceInfo(
            "_ohnx-sync._tcp.local.",
            "OHNX-Hub-%s._ohnx-sync._tcp.local." % socket.gethostname().split(".")[0],
            addresses=[socket.inet_aton(lip)] if lip else None,
            port=a.port,
            properties={"v": "1", "api": "/ohnx",
                        "fb": "1" if a.compat_port else "0",
                        "role": "hub", "uuid": hubUuid,
                        "dev": socket.gethostname().split(".")[0]},
            server=socket.gethostname() + ".local.",
        )
        zc = Zeroconf()
        zc.register_service(svc)
        print("ohnx hub: mDNS _ohnx-sync._tcp pubblicato (v=1 api=/ohnx fb=%s)"
              % ("1" if a.compat_port else "0"), flush=True)
    except ImportError:
        print("ohnx hub: zeroconf assente, nessun publish mDNS", flush=True)
    except Exception as e:
        # Nome duplicato (secondo hub stesso hostname), rete senza
        # multicast, ecc: mai morire per il publish, l'HTTP resta su.
        print("ohnx hub: publish mDNS fallito (%s: %s)" % (type(e).__name__, e),
              flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if zc is not None:
            try:
                zc.unregister_all_services()
                zc.close()
            except Exception:
                pass


if __name__ == "__main__":
    main()
