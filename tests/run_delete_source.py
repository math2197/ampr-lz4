#!/usr/bin/env python3
"""Teste de integração da opção delete_source (apagar a imagem original), no host:
build/ampr-lz4-host + tools/mock_smp.py, com AMPR_ROOT_PREFIX numa pasta temporária.

Cobre: sucesso apagando; desmonte falhando (não apaga); desmonte "ok" mas imagem ainda montada
(não apaga); verificação falhando (não apaga, AMPR_HOST_FAIL_VERIFY); imagem symlink (não apaga);
sem delete_source (não apaga); validação de keep_loose/delete_source/pack_all em /api/convert.
Rode com TMPDIR apontando para um diretório de rascunho. GPL-3.0."""
from __future__ import annotations

import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
HOST = ROOT / "build" / "ampr-lz4-host"
MOCK = ROOT / "tools" / "mock_smp.py"


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def http(method: str, url: str, body: dict | None = None) -> tuple[int, dict]:
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def wait_port(port: int, proc: subprocess.Popen, log: Path) -> None:
    for _ in range(100):
        if proc.poll() is not None:
            raise SystemExit(f"processo saiu (rc={proc.returncode}):\n{log.read_text(errors='replace')[-3000:]}")
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return
        except OSError:
            time.sleep(0.1)
    raise SystemExit(f"porta {port} não abriu:\n{log.read_text(errors='replace')[-3000:]}")


def w(root: Path, rel: str, data: bytes) -> None:
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)


def make_runtime(d: Path) -> None:
    """Árvore do jogo "montado" que o mock devolve em runtime_path."""
    if d.exists():
        shutil.rmtree(d)
    w(d, "eboot.bin", b"\x7fELF" + b"\0" * 1000 + b"libSceAmpr.sprx\0" + os.urandom(3000))
    w(d, "sce_sys/param.json", b'{"titleId":"PPSA01001"}')
    w(d, "Game/Content/Paks/pakchunk0-ps5.ucas", b"ucas data " * 20000)
    w(d, "Game/Content/Paks/pakchunk0-ps5.utoc", b"utoc data " * 2000)
    w(d, "Data/level.res", b"level data " * 30000)


class Env:
    def __init__(self, t: Path):
        self.t = t
        self.prefix = t / "root"
        self.rt = t / "rt"
        self.mock_port = free_port()
        self.logs = t / "logs"
        self.logs.mkdir()
        (self.prefix / "data/etaHEN/games").mkdir(parents=True)
        (self.prefix / "mnt/ext0/games").mkdir(parents=True)
        self.rt.mkdir()
        mlog = self.logs / "mock.log"
        self.mock = subprocess.Popen([sys.executable, "-I", str(MOCK), str(self.mock_port), str(self.rt)],
                                     stdout=mlog.open("wb"), stderr=subprocess.STDOUT)
        wait_port(self.mock_port, self.mock, mlog)
        self.host = None
        self.port = 0

    def start_host(self, extra_env: dict | None = None) -> None:
        self.stop_host()
        self.port = free_port()
        env = dict(os.environ, AMPR_ROOT_PREFIX=str(self.prefix), AMPR_PORT=str(self.port),
                   AMPR_SMP_PORT=str(self.mock_port))
        env.pop("AMPR_HOST_FAIL_VERIFY", None)
        env.update(extra_env or {})
        self.hlog = self.logs / f"host-{self.port}.log"
        self.host = subprocess.Popen([str(HOST)], env=env, stdout=self.hlog.open("wb"), stderr=subprocess.STDOUT)
        wait_port(self.port, self.host, self.hlog)

    def stop_host(self) -> None:
        if self.host and self.host.poll() is None:
            self.host.terminate()
            self.host.wait(10)
        self.host = None

    def close(self) -> None:
        self.stop_host()
        self.mock.terminate()
        self.mock.wait(10)

    def api(self, method: str, path: str, body: dict | None = None) -> tuple[int, dict]:
        return http(method, f"http://127.0.0.1:{self.port}{path}", body)

    def control(self, **kw) -> None:
        st, _ = http("POST", f"http://127.0.0.1:{self.mock_port}/mock/control", kw)
        assert st == 200

    def smp_unmount(self, tid: str) -> None:
        http("POST", f"http://127.0.0.1:{self.mock_port}/api/v1/games/unmount", {"title_id": tid})

    def convert(self, body: dict) -> dict:
        st, r = self.api("POST", "/api/convert", body)
        assert st == 200 and r.get("ok"), (st, r)
        job = r["job"]
        for _ in range(600):
            _, s = self.api("GET", "/api/status")
            for h in s.get("history", []):
                if h["job"] == job:
                    return h
            time.sleep(0.1)
        raise SystemExit("job não terminou:\n" + self.hlog.read_text(errors="replace")[-3000:])


def image(env: Env, logical: str, size: int = 1 << 20) -> Path:
    p = env.prefix / logical.lstrip("/")
    p.write_bytes(os.urandom(size))
    return p


def main() -> int:
    if not HOST.exists():
        raise SystemExit(f"{HOST} não existe: rode `make host` na raiz antes")
    with tempfile.TemporaryDirectory(prefix="ampr-del-") as td:
        env = Env(Path(td))
        try:
            env.start_host()
            dst = env.prefix / "data/homebrew/PPSA01001-AMPR"

            # validação de entrada
            for bad, why in (({"keep_loose": ".ucas;rm"}, "caractere"), ({"keep_loose": "a" * 256}, "256 chars"),
                             ({"keep_loose": 5}, "não-string"), ({"delete_source": "yes"}, "delete_source string"),
                             ({"pack_all": 1}, "pack_all número"), ({"pack_all": "yes"}, "pack_all string")):
                st, r = env.api("POST", "/api/convert", dict({"id": "PPSA01001", "dest_drive": "data"}, **bad))
                assert st == 400 and not r.get("ok"), (why, st, r)
            print("validação ok: keep_loose inválido e delete_source/pack_all não-booleanos recusados (400)")

            # 1) sucesso apagando
            make_runtime(env.rt / "PPSA01001")
            img = image(env, "/data/etaHEN/games/jogo1.exfat", 3 << 20)
            h = env.convert({"id": "PPSA01001", "dest_drive": "data", "level": 4, "delete_source": True,
                             "keep_loose": ".res,Data/"})
            assert h["result"] == "done", h
            assert h["source_deleted"] is True and h["freed_bytes"] == 3 << 20, h
            assert not img.exists() and img.parent.is_dir(), "imagem não foi apagada (ou apagou a pasta)"
            assert (dst / "ampr_assets.index").is_file()
            assert not (dst / "Game/Content/Paks/pakchunk0-ps5.ucas").exists(), "ucas deveria ir para os pacotes (unreal)"
            assert (dst / "Game/Content/Paks/pakchunk0-ps5.utoc").is_file(), "utoc deveria ficar solto (unreal)"
            assert (dst / "Data/level.res").is_file(), "keep_loose .res não aplicado"
            assert h["engine"] == "unreal", h
            _, g = env.api("GET", "/api/games")
            eng = {x["id"]: x.get("engine") for x in g["games"]}
            assert eng.get("PPSA01001") == "unreal", eng
            log = env.hlog.read_text(errors="replace")
            assert "original apagado" in log and "imagem original apagada" in log, log[-2000:]
            print(f"sucesso: imagem apagada, freed_bytes={h['freed_bytes']}, engine={h['engine']}, "
                  "notificação 'original apagado' emitida")

            # 2) desmonte falhando: não apaga, conclui com aviso
            shutil.rmtree(dst)
            img = image(env, "/data/etaHEN/games/jogo1.exfat")
            env.control(unmount_fail=True)
            h = env.convert({"id": "PPSA01001", "dest_drive": "data", "delete_source": True})
            env.control(unmount_fail=False)
            env.smp_unmount("PPSA01001")
            assert h["result"] == "done" and h["source_deleted"] is False, h
            assert h["warning"].startswith("original mantido: falha ao desmontar"), h
            assert img.exists(), "imagem apagada apesar da falha no desmonte"
            print(f"desmonte falhando: não apagou ({h['warning']})")

            # 3) desmonte responde ok, mas a imagem continua montada: não apaga
            shutil.rmtree(dst)
            env.control(stay_mounted=True)
            h = env.convert({"id": "PPSA01001", "dest_drive": "data", "delete_source": True})
            env.control(stay_mounted=False)
            env.smp_unmount("PPSA01001")
            assert h["result"] == "done" and h["source_deleted"] is False, h
            assert "ainda consta como montada" in h["warning"], h
            assert img.exists()
            print(f"ainda montada: não apagou ({h['warning']})")

            # 4) sem delete_source: não apaga, sem aviso
            shutil.rmtree(dst)
            h = env.convert({"id": "PPSA01001", "dest_drive": "data", "pack_all": True})
            assert h["result"] == "done" and h["source_deleted"] is False and h["warning"] == "", h
            assert img.exists()
            log = env.hlog.read_text(errors="replace")
            assert "job: pack_all=1" in log and "pack_all ativo" in log, log[-2000:]
            print("sem delete_source: imagem mantida; pack_all=true do pedido chegou ao núcleo")

            # 5) imagem symlink: não apaga nem o link nem o alvo
            make_runtime(env.rt / "PPSA02002")
            target = env.t / "outside.ffpfsc"
            target.write_bytes(b"x" * 4096)
            link = env.prefix / "mnt/ext0/games/jogo2.ffpfsc"
            os.symlink(target, link)
            h = env.convert({"id": "PPSA02002", "dest_drive": "ext0", "delete_source": True})
            assert h["result"] == "done" and h["source_deleted"] is False and "symlink" in h["warning"], h
            assert link.is_symlink() and target.exists()
            print(f"symlink: não apagou ({h['warning']})")

            # 6) verificação falhando: job em erro, não apaga, destino removido
            shutil.rmtree(dst)
            env.start_host({"AMPR_HOST_FAIL_VERIFY": "1"})
            h = env.convert({"id": "PPSA01001", "dest_drive": "data", "delete_source": True})
            assert h["result"] == "error" and h["source_deleted"] is False, h
            assert "falha simulada" in h["error"], h
            assert img.exists() and not dst.exists()
            print(f"verificação falhando: não apagou (erro: {h['error']})")
        except BaseException:
            for f in sorted(env.logs.iterdir()):
                print(f"--- {f.name} ---\n" + f.read_text(errors="replace")[-2500:])
            raise
        finally:
            env.close()
    print("DELETE_SOURCE OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
