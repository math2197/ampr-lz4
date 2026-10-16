#!/usr/bin/env python3
"""Mock da API do payload AMPR-LZ4. Uso: python3 -I mock_server.py [porta]"""
import json, os, struct, sys, threading, time, zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

UI = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
G = 1073741824
GAMES = [
    ("g1", "Astro Rescue Mission", "PPSA01234", "exfat", "data", 42 * G, False, True),
    ("g2", "The Legend of a Very Long Adventure Title: Definitive Director's Cut Edition", "PPSA05678", "exfat", "data", 88 * G, False, True),
    ("g3", "Speed Racers 2077", "PPSA09999", "ffpfsc", "ext0", 61 * G, False, True),
    ("g4", "Sem Icone Game", "CUSA00001", "exfat", "data", 12 * G, False, True),
    ("g5", "Cozy Farm Valley", "PPSA02222", "exfat", "ext1", 7 * G, True, True),
    ("g6", "Incompat Online Shooter", "PPSA03333", "exfat", "data", 95 * G, False, False),
    ("g7", "Dragon Souls Remastered", "PPSA04444", "ffpfsc", "ext0", 53 * G, False, True),
    ("g8", "Pixel Quest", "CUSA05555", "exfat", "ext0", 2 * G + G // 2, False, True),
]
STAGES = ["mount", "scan", "pack", "copy", "verify", "finish"]
lock = threading.Lock()
state = {"cur": None, "queue": [], "hist": [], "n": 0, "converted": {"g5"}, "engine": {"g5": "unity"}}
# engine simulada e fração empacotada por jogo (g7: quase tudo solto, mostra o aviso)
ENG = {"g1": ("unreal", 0.55), "g2": ("unreal", 0.6), "g3": ("unknown", 0.9), "g4": ("unity", 0.8),
       "g6": ("unknown", 0.9), "g7": ("northlight", 0.05), "g8": ("unity", 0.85)}

def png(r, g, b, n=64):
    raw = b''.join(b'\x00' + bytes(v for x in range(n) for v in (min(255, int(r + x * 2)), int(g * (1 - y / n)), int(b))) for y in range(n))
    def ch(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 2, 0, 0, 0)) + ch(b"IDAT", zlib.compress(raw)) + ch(b"IEND", b"")

def worker():
    while True:
        time.sleep(0.3)
        with lock:
            c = state["cur"]
            if c is None:
                if state["queue"]:
                    j = state["queue"].pop(0)
                    state["cur"] = c = dict(j, stage="mount", bytes_done=0, bytes_total=j["src"], src_bytes=0, out_bytes=0, speed_bps=0, eta_s=0, error="", t=0, cancel=False,
                                            engine=None, packed_bytes=0, loose_bytes=0, auto_loose_bytes=0)
                else: continue
            if c["cancel"]:
                finish(c, "cancelled"); continue
            c["t"] += 1
            t = c["t"]  # ~0.3 s por tick, 6 etapas x 8 ticks
            idx = min(5, t // 8)
            c["stage"] = STAGES[idx]
            c["bytes_total"] = c["src"]
            c["bytes_done"] = int(c["src"] * min(1, t / 48))
            c["speed_bps"] = 350 * 1048576
            c["eta_s"] = (48 - t) * 20
            if idx >= 2:
                e, frac = ENG.get(c["id"], ("unknown", 0.8))
                c["engine"] = e; c["profile"] = "unity" if e == "unity" else None; c["packed_bytes"] = int(c["src"] * frac); c["loose_bytes"] = c["src"] - c["packed_bytes"]
                c["auto_loose_bytes"] = 0 if c.get("pack_all") else int(c["packed_bytes"] * 0.15)
            if idx >= 2: c["src_bytes"] = c["src"]; c["out_bytes"] = int(c["src"] * 0.62 * min(1, (t - 14) / 20 if t > 14 else 0.1))
            if c["title_id"] == "PPSA03333" and False: finish(c, "error")
            if t >= 48:
                c["out_bytes"] = int(c["src"] * 0.62); finish(c, "done")

def finish(c, stage):
    deleted = stage == "done" and c.get("delete_source", False)
    state["hist"].append({"job": c["job"], "id": c["id"], "title": c["title"], "result": stage, "src_bytes": c["src"] if stage == "done" else 0,
                          "out_bytes": c["out_bytes"] if stage == "done" else 0, "error": c["error"], "engine": c.get("engine"),
                          "profile": c.get("profile"), "source_deleted": deleted, "freed_bytes": c["src"] if deleted else 0, "warning": ""})
    if c.get("engine"): state["engine"][c["id"]] = c["engine"]
    if stage == "done": state["converted"].add(c["id"])
    state["cur"] = None

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, body, ctype="application/json"):
        if not isinstance(body, bytes): body = json.dumps(body).encode()
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        p, _, q = self.path.partition("?")
        if p in ("/", "/app.css", "/app.js"):
            f = "index.html" if p == "/" else p[1:]
            ct = {"html": "text/html; charset=utf-8", "css": "text/css", "js": "application/javascript"}[f.rsplit(".", 1)[1]]
            return self.send(200, open(os.path.join(UI, f), "rb").read(), ct)
        if p == "/api/info": return self.send(200, {"version": "1.0.0", "smp_version": "1.4.2", "ip": "192.168.0.50", "port": 8080})
        if p == "/api/games":
            with lock:
                return self.send(200, {"games": [{"id": i, "title": t, "title_id": tid, "format": f, "drive": d, "image_path": "/%s/homebrew/%s.%s" % (d, tid, f),
                    "size_bytes": s, "converted": i in state["converted"], "compatible": None if comp else False,
                    "dest_path": "/%s/ampr/%s" % (d, tid) if i in state["converted"] else "", "engine": state["engine"].get(i)}
                    for i, t, tid, f, d, s, _c, comp in GAMES]})
        if p == "/api/icon":
            gid = q.split("=")[-1]
            if gid == "g4": return self.send(404, b"", "text/plain")
            n = int(gid[1:]) if gid[1:].isdigit() else 1
            return self.send(200, png(30 * n % 200, 60 + 20 * n, 255 - 25 * n), "image/png")
        if p == "/api/status":
            with lock:
                c = state["cur"]
                cur = None if c is None else {k: c[k] for k in ("job", "id", "title", "stage", "bytes_done", "bytes_total", "src_bytes", "out_bytes", "speed_bps", "eta_s", "error",
                                                       "engine", "packed_bytes", "loose_bytes", "auto_loose_bytes", "pack_all", "delete_source")}
                if cur is not None: cur["profile"] = c.get("profile")
                return self.send(200, {"current": cur, "queue": [{"job": j["job"], "id": j["id"], "title": j["title"]} for j in state["queue"]], "history": state["hist"][-10:]})
        self.send(404, {"ok": False, "error": "não encontrado"})
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); d = json.loads(self.rfile.read(n) or b"{}")
        with lock:
            if self.path == "/api/convert":
                g = next((x for x in GAMES if x[0] == d.get("id")), None)
                if not g: return self.send(200, {"ok": False, "error": "Jogo não encontrado"})
                if g[0] in state["converted"]: return self.send(200, {"ok": False, "error": "O destino já existe: /%s/ampr/%s" % (d.get("dest_drive"), g[2])})
                kl = d.get("keep_loose", "")
                if not isinstance(kl, str) or len(kl) > 255 or any(ch not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._/,-" for ch in kl):
                    return self.send(400, {"ok": False, "error": "keep_loose: até 255 caracteres de A-Z a-z 0-9 . _ / , -"})
                if not isinstance(d.get("delete_source", False), bool):
                    return self.send(400, {"ok": False, "error": "delete_source deve ser true ou false"})
                if not isinstance(d.get("pack_all", False), bool):
                    return self.send(400, {"ok": False, "error": "pack_all deve ser true ou false"})
                state["n"] += 1
                state["queue"].append({"job": state["n"], "id": g[0], "title": g[1], "title_id": g[2], "src": g[5],
                                       "delete_source": d.get("delete_source", False), "pack_all": d.get("pack_all", False), "keep_loose": kl})
                return self.send(200, {"ok": True, "job": state["n"]})
            if self.path == "/api/cancel":
                if state["cur"] and state["cur"]["job"] == d.get("job"): state["cur"]["cancel"] = True
                return self.send(200, {"ok": True})
        self.send(404, {"ok": False})

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    threading.Thread(target=worker, daemon=True).start()
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
