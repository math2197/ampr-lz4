#!/usr/bin/env python3
"""Mock do ShadowMountPlus 1.7 (API /api/v1) para testar o payload no host.
Uso: python3 -I tools/mock_smp.py [porta=10101] [pasta_runtime]
O mount devolve (via /games/info) runtime_path apontando para uma pasta local com eboot.bin.
Testes: POST /mock/control {"unmount_fail": bool, "stay_mounted": bool} simula falha no desmonte
(HTTP 500) ou desmonte que responde ok mas a imagem continua montada."""
import json, os, struct, sys, zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 10101
RT = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else "mock_runtime")

def png(rgb):
    w = h = 64
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))

GAMES = {
    "PPSA01001": dict(title_name="Jogo Exfat (data)", path="/data/etaHEN/games/jogo1.exfat", image_type="exfatfs", color=(200, 60, 60), size=21474836480),
    "PPSA02002": dict(title_name="Jogo FFPFSC (ext0)", path="/mnt/ext0/games/jogo2.ffpfsc", image_type="pfsc", color=(60, 160, 60), size=53687091200),
    "PPSA03003": dict(title_name="Jogo Já Convertido", path="/mnt/ext0/games/jogo3.exfat", image_type="exfatfs", color=(60, 60, 200), size=10737418240),
    "PPSA04004": dict(title_name="Jogo em Pasta (ignorado)", path="/data/homebrew/pasta", image_type="", color=(150, 150, 0), size=1000, folder=True),
}
STATE = {t: {"mounted": False, "fakelib": "disabled" if t == "PPSA01001" else "full"} for t in GAMES}
CONTROL = {"unmount_fail": False, "stay_mounted": False}

def game(t):
    g, s = GAMES[t], STATE[t]
    return {"path": g["path"], "runtime_path": os.path.join(RT, t) if s["mounted"] else "",
            "source_type": "folder" if g.get("folder") else "image", "image_type": g["image_type"], "platform": "ps5",
            "title_id": t, "content_id": "EP0000-%s_00-X" % t, "title_name": g["title_name"], "version": "01.000",
            "icon_url": "/api/v1/games/icon?title_id=" + t, "app_db_size_bytes": g["size"], "size_bytes": g["size"],
            "size_status": 0, "installed": True, "managed": True, "mounted": s["mounted"], "image_backed": not g.get("folder"),
            "source_available": True, "installed_pkg": False, "fakelib_mode": s["fakelib"],
            "fakelib_mode_override": "default", "fakelib_default_mode": "full", "fakelib_enabled": s["fakelib"] != "disabled",
            "fakelib_effective_enabled": True}

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, f, *a): sys.stderr.write("[mock-smp] " + f % a + "\n")
    def send(self, code, obj=None, raw=None, ctype="application/json"):
        body = raw if raw is not None else json.dumps(obj).encode()
        self.send_response(code); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body))); self.send_header("Connection", "close")
        self.end_headers(); self.wfile.write(body); self.close_connection = True
    def err(self, code, msg): self.send(code, {"status": code, "error": msg})
    def do_GET(self):
        if self.path.startswith("/api/v1/games/icon"):
            t = self.path.split("title_id=")[-1].split("&")[0]
            if t in GAMES: return self.send(200, raw=png(GAMES[t]["color"]), ctype="image/png")
        self.err(404, "not found")
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        try: req = json.loads(self.rfile.read(n) or b"{}")
        except ValueError: return self.err(400, "bad json")
        p, t = self.path, req.get("title_id")
        if p == "/mock/control":
            for k in CONTROL:
                if k in req: CONTROL[k] = bool(req[k])
            return self.send(200, dict(CONTROL, status=0))
        if p == "/api/v1/version":
            return self.send(200, {"status": 0, "api_version": 1, "shadowmount_version": "1.7-mock", "capabilities": []})
        if p == "/api/v1/games":
            gs = [game(x) for x in GAMES]
            return self.send(200, {"status": 0, "count": len(gs), "size_included": bool(req.get("include_size")), "games": gs})
        if p in ("/api/v1/games/info", "/api/v1/games/mount", "/api/v1/games/unmount", "/api/v1/games/fakelib"):
            if t not in GAMES: return self.err(404, "unknown title")
            if p.endswith("/info"):
                d = game(t); d["status"] = 0; return self.send(200, d)
            if p.endswith("/mount"):
                if req.get("mode") == "rw": return self.err(501, "mode not supported")
                d = os.path.join(RT, t); os.makedirs(os.path.join(d, "sce_sys"), exist_ok=True)
                if not os.path.exists(os.path.join(d, "eboot.bin")): open(os.path.join(d, "eboot.bin"), "wb").close()
                STATE[t]["mounted"] = True
                return self.send(200, {"status": 0, "title_id": t, "mounted": True, "mode": req.get("mode", "default")})
            if p.endswith("/unmount"):
                if CONTROL["unmount_fail"]: return self.err(500, "unmount failed (mock)")
                if not CONTROL["stay_mounted"]: STATE[t]["mounted"] = False
                return self.send(200, {"status": 0, "title_id": t, "mounted": False})
            m = req.get("mode")
            if m not in ("full", "emulators", "disabled", "default"): return self.err(400, "bad mode")
            STATE[t]["fakelib"] = "full" if m == "default" else m
            return self.send(200, {"status": 0, "title_id": t, "fakelib_mode": STATE[t]["fakelib"], "saved": True, "applies_on_next_launch": True})
        self.err(404, "not found")

if __name__ == "__main__":
    os.makedirs(RT, exist_ok=True)
    print("mock SMP em 127.0.0.1:%d, runtime em %s" % (PORT, RT), flush=True)
    ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
