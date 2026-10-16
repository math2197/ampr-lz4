#!/usr/bin/env python3
"""Teste ponta a ponta sintético: gera jogos falsos, converte com build/host_convert e valida.
Rode com TMPDIR apontando para um diretório de rascunho. GPL-3.0."""
from __future__ import annotations

import os
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
CONV = HERE / "build" / "host_convert"
VALIDATE = HERE / "validate_ampr.py"


def w(root: Path, rel: str, data: bytes) -> None:
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)


def text_blob(rng: random.Random, n: int) -> bytes:
    words = [b"alpha", b"beta", b"gamma", b"delta", b"mesh", b"texture", b"level", b"\x00\x01\x02\x03"]
    out = bytearray()
    while len(out) < n:
        out += rng.choice(words) + b" "
    return bytes(out[:n])


def make_game(root: Path, big: bool) -> None:
    rng = random.Random(1234)
    w(root, "eboot.bin", b"\x7fELF" + b"\0" * 1000 + b"libSceAmpr.sprx\0" + os.urandom(5000))
    w(root, "sce_sys/param.json", b'{"titleId":"PPSA00000"}')
    w(root, "sce_sys/icon0.png", os.urandom(3000))
    w(root, "sce_module/libc.prx", os.urandom(20000))
    w(root, "root_file.txt", b"hello")
    w(root, "Data/Level1.ARC", text_blob(rng, 3 * 65536 + 123))          # comprime, 4 blocos
    w(root, "Data/exact.bin2", text_blob(rng, 2 * 65536))                 # múltiplo exato
    w(root, "Data/random.arc", os.urandom(200000))                        # RAW
    w(root, "Data/dup_a.res", text_blob(random.Random(7), 150000))        # dedup
    w(root, "Data/dup_b.res", text_blob(random.Random(7), 150000))
    w(root, "Data/tiny.cfg", b"x" * 10)
    w(root, "Data/empty.res", b"")
    w(root, "Data/movie.mp4", os.urandom(70000))
    w(root, "Data/archive.PAK", os.urandom(1000))
    w(root, "Data/Sub Dir/çÃo.res", text_blob(rng, 40000))
    w(root, "Sound/bank.bnk", text_blob(rng, 100000))                     # 16 KiB
    w(root, "Sound/sfx/hit.at9", os.urandom(5000))
    w(root, "Mixed/Weird Name.dat2", text_blob(rng, 5000))
    (root / "EmptyDir/Nested").mkdir(parents=True)
    w(root, "ampr_commands.bin", b"log")                                  # excluído
    if big:  # auto-loose: >= 64 MiB e incompressível
        w(root, "Data/huge_random.big", os.urandom(65 << 20))
        w(root, "Data/huge_text.big", text_blob(rng, 1 << 20) * 66)


def idx_sizes(path: Path) -> dict[str, int]:
    """Lê o AMPRIDX3 (ampr_emu.index): {caminho /app0/... em minúsculas: tamanho}."""
    data = path.read_bytes()
    magic, _v, _es, n, path_bytes, _ho, _ss, _sl = struct.unpack_from("<8sIIQQQII", data)
    assert magic == b"AMPRIDX3", magic
    blob = data[48 + n * 24 : 48 + n * 24 + path_bytes]
    out = {}
    for i in range(n):
        at, ln, size, _mtime = struct.unpack_from("<IIQq", data, 48 + i * 24)
        out[blob[at : at + ln].decode("utf-8").lower()] = size
    return out


def run(cmd: list[str], expect_ok: bool = True) -> str:
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = r.stdout + r.stderr
    if (r.returncode == 0) != expect_ok:
        print(out)
        raise SystemExit(f"falhou: {' '.join(map(str, cmd))} rc={r.returncode}")
    return out


def main() -> int:
    big = "--big" in sys.argv
    with tempfile.TemporaryDirectory(prefix="ampr-e2e-") as td:
        t = Path(td)
        src = t / "game"
        make_game(src, big)
        fake = t / "libSceAmpr.sprx"
        fake.write_bytes(b"FAKELIB" * 100)

        for level, workers in ((1, 1), (9, 4), (12, 3)):
            dst = t / f"out-{level}"
            out = run([str(CONV), "-l", str(level), "-w", str(workers), "-f", str(fake), str(src), str(dst)])
            print(out.strip().splitlines()[-1])
            print(run([sys.executable, "-I", str(VALIDATE), str(src), str(dst), "--fakelib", str(fake)]).strip())

        dst = t / "out-nofake"
        run([str(CONV), "-l", "5", str(src), str(dst)])
        assert "VALIDAÇÃO OK" in run([sys.executable, "-I", str(VALIDATE), str(src), str(dst)])
        print("sem fakelib: VALIDAÇÃO OK")

        # rollover de volume (host: AMPR_HOST_MAX_VOLUME)
        env = dict(os.environ, AMPR_HOST_MAX_VOLUME=str(128 << 10))
        r = subprocess.run([str(CONV), "-l", "9", str(src), str(t / "out-roll")], capture_output=True, text=True, env=env)
        assert r.returncode == 0, r.stdout + r.stderr
        vols = sorted(p.name for p in (t / "out-roll").glob("ampr_assets-*.pak"))
        assert len(vols) >= 3, vols
        assert all((t / "out-roll" / v).stat().st_size <= 128 << 10 for v in vols)
        vout = run([sys.executable, "-I", str(VALIDATE), str(src), str(t / "out-roll")])
        assert "VALIDAÇÃO OK" in vout, vout
        print(f"rollover ok: {len(vols)} volumes de <= 128 KiB validados")

        # determinismo: mesma entrada -> manifesto idêntico
        run([str(CONV), "-l", "5", "-w", "7", str(src), str(t / "out-nofake2")])
        a = (dst / "ampr_assets.index").read_bytes()
        b = (t / "out-nofake2" / "ampr_assets.index").read_bytes()
        assert a == b, "manifesto não determinístico"
        print("determinismo ok (workers 4 vs 7)")

        # negativos
        out = run([str(CONV), str(src), str(dst)], expect_ok=False)
        assert "já existe" in out and (dst / "ampr_assets.index").exists(), out
        bad = t / "noampr"
        bad.mkdir()
        (bad / "eboot.bin").write_bytes(b"nothing here")
        out = run([str(CONV), "-c", str(bad)], expect_ok=False)
        assert "libSceAmpr" in out, out
        (bad / "sce_module").mkdir()
        (bad / "sce_module" / "x.sprx").write_bytes(b"..libSceAmpr..")
        run([str(CONV), "-c", str(bad)])
        out = run([str(CONV), "-c", str(dst)], expect_ok=False)
        assert "já está em AMPR" in out, out
        sl = t / "symgame"
        make_game(sl, False)
        os.symlink("eboot.bin", sl / "Data" / "link.res")
        out = run([str(CONV), str(sl), str(t / "out-sym")], expect_ok=False)
        assert "symlink" in out and not (t / "out-sym").exists(), out
        bs = t / "bsgame"
        make_game(bs, False)
        w(bs, "Data/a\\b.res", b"x")
        out = run([str(CONV), str(bs), str(t / "out-bs")], expect_ok=False)
        assert "não suportado" in out and not (t / "out-bs").exists(), out
        if big:
            out = run([str(CONV), "-l", "12", "-w", "1", "-x", "300", str(src), str(t / "out-cancel")], expect_ok=False)
            assert "CANCELLED" in out and not (t / "out-cancel").exists(), out
            print("cancelamento ok (stage CANCELLED, destino removido)")
        print("negativos ok (destino existente, sem libSceAmpr, já AMPR, symlink + limpeza)")

        # origem com fakelib/libSceAmpr.sprx ANTIGO (só AMPRIDX3): o destino deve receber o sprx passado
        # em fakelib_sprx (-f), com o tamanho dele no índice; os demais .sprx da fakelib seguem copiados.
        og = t / "oldfake"
        make_game(og, False)
        old_sprx = b"\x7fELF old AMPRIDX3 " + b"o" * 3000
        agc = b"\x7fELF libSceAgc " + os.urandom(4000)
        w(og, "fakelib/libSceAmpr.sprx", old_sprx)
        w(og, "fakelib/libSceAgc.sprx", agc)
        new_sprx = t / "new" / "libSceAmpr.sprx"
        new_sprx.parent.mkdir()
        new_sprx.write_bytes(b"\x7fELF new AMPRIDX3 AMPRPAK4 AMPRDAT3 " + os.urandom(9000))
        od = t / "out-oldfake"
        run([str(CONV), "-l", "5", "-f", str(new_sprx), str(og), str(od)])
        vout = run([sys.executable, "-I", str(VALIDATE), str(og), str(od), "--fakelib", str(new_sprx)])
        assert "VALIDAÇÃO OK" in vout, vout
        got = (od / "fakelib" / "libSceAmpr.sprx").read_bytes()
        assert got == new_sprx.read_bytes(), "fakelib/libSceAmpr.sprx do destino não é o passado em -f"
        assert (od / "fakelib" / "libSceAgc.sprx").read_bytes() == agc, "libSceAgc.sprx da origem não foi copiado"
        sizes = idx_sizes(od / "ampr_emu.index")
        key = "/app0/fakelib/libsceampr.sprx"
        assert sizes.get(key) == len(got) != len(old_sprx), (sizes.get(key), len(got), len(old_sprx))
        print(f"fakelib antigo substituído: destino tem o sprx novo ({len(got)} bytes no índice), libSceAgc copiado")
    print("E2E OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
