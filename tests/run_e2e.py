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
    w(root, "Sound/ui_bank.pck", text_blob(rng, 70000))                   # áudio: HOT|RANDOM_ACCESS, 16 KiB
    w(root, "Data/soundbank.pck", text_blob(rng, 30000))                  # soundbank.*: HOT|RANDOM_ACCESS
    w(root, "Mixed/Weird Name.dat2", text_blob(rng, 5000))
    (root / "EmptyDir/Nested").mkdir(parents=True)
    w(root, "ampr_commands.bin", b"log")                                  # excluído
    if big:  # auto-loose: >= 64 MiB e incompressível
        w(root, "Data/huge_random.big", os.urandom(65 << 20))
        w(root, "Data/huge_text.big", text_blob(rng, 1 << 20) * 66)


def make_base(root: Path, seed: int, title_id: str = "PPSA00001", extra: bytes = b"") -> random.Random:
    rng = random.Random(seed)
    w(root, "eboot.bin", b"\x7fELF" + b"\0" * 1000 + b"libSceAmpr.sprx\0" + extra + os.urandom(5000))
    w(root, "sce_sys/param.json", ('{"titleId":"%s","localizedParameters":{"defaultLanguage":"en-US",'
                                   '"en-US":{"titleName":"Synthetic Test"}}}' % title_id).encode())
    return rng


def make_unreal(root: Path) -> None:
    """Árvore estilo Unreal (IoStore): .ucas comum vai para os pacotes; .utoc/.pak/.sig e o container
    global ficam soltos (perfil Unreal + lista de segurança)."""
    rng = make_base(root, 99)
    w(root, "Game/Content/Paks/pakchunk0-ps5.pak", text_blob(rng, 50000))     # solto (.pak, segurança)
    w(root, "Game/Content/Paks/pakchunk0-ps5.utoc", text_blob(rng, 20000))    # solto (.utoc, segurança)
    w(root, "Game/Content/Paks/pakchunk0-ps5.ucas", text_blob(rng, 300000))   # empacotado
    w(root, "Game/Content/Paks/pakchunk1-ps5.ucas", text_blob(rng, 150000))   # empacotado
    w(root, "Game/Content/Paks/pakchunk0-ps5.sig", text_blob(rng, 3000))      # solto (.sig, perfil Unreal)
    w(root, "Engine/Content/Paks/global.ucas", text_blob(rng, 200000))        # solto (container global)
    w(root, "Engine/Content/Paks/global.utoc", text_blob(rng, 9000))          # solto
    w(root, "Game/Content/Paks/Sub/GLOBAL.UCAS", text_blob(rng, 60000))       # solto (nome, sem maiúsculas)
    w(root, "Game/Content/Maps/Level.uasset", text_blob(rng, 70000))          # empacotado
    w(root, "Game/Content/Maps/Level.ubulk", text_blob(rng, 90000))           # empacotado
    w(root, "Game/Content/Movies/intro.dat2", text_blob(rng, 120000))   # empacotado
    w(root, "Game/Config/DefaultGame.ini", text_blob(rng, 40000))       # solto (.ini, segurança)
    w(root, "Engine/Config/BaseEngine.ini", text_blob(rng, 30000))      # solto (.ini)
    w(root, "Engine/Config/PS5/PS5Engine.ini", text_blob(rng, 9000))    # solto (.ini)
    w(root, "Game/Config/Tags.cfgx", text_blob(rng, 9000))              # solto (pasta Config/, perfil Unreal)
    w(root, "Game/Game.uproject", text_blob(rng, 9000))                 # solto (.uproject)
    w(root, "Game/Plugins/Foo/Foo.uplugin", text_blob(rng, 9000))       # solto (.uplugin)
    w(root, "Game/Binaries/Game.upluginmanifest", text_blob(rng, 9000)) # solto (.upluginmanifest)
    w(root, "Game/Content/Localization/Game/en/Game.locres", text_blob(rng, 20000))  # solto
    w(root, "Game/Content/Localization/Game/Game.locmeta", text_blob(rng, 9000))     # solto
    w(root, "Engine/Content/Internationalization/icudt64l/coll/root.res", text_blob(rng, 20000))  # ICU: solto
    w(root, "Game/Content/Data/blob.dat2", text_blob(rng, 80000))       # empacotado


def make_northlight(root: Path) -> None:
    rng = make_base(root, 77)
    w(root, "data/ep100.rmdtoc", text_blob(rng, 80000))
    w(root, "data/ep100-000.rmdp", text_blob(rng, 250000))             # empacotado
    w(root, "data/streaming.rmdtoc", text_blob(rng, 30000))


def make_unity(root: Path) -> None:
    rng = make_base(root, 55)
    w(root, "Media/Data/globalgamemanagers", text_blob(rng, 60000))
    w(root, "Media/Data/sharedassets0.assets", text_blob(rng, 90000))
    w(root, "Media/Data/sharedassets0.resS", text_blob(rng, 90000))


def make_insomniac(root: Path) -> None:
    """Estilo Wolverine (PPSA03671): toc, dag e d/ na raiz; só d/* empacotado (perfil insomniac, por layout)."""
    rng = make_base(root, 33)
    w(root, "toc", text_blob(rng, 40000))
    w(root, "dag", text_blob(rng, 40000))
    w(root, "commandline.txt", b"-x")
    w(root, "d/gfx/archive_000", text_blob(rng, 300000))                # empacotado, blocos de 128 KiB
    w(root, "d/gfx/archive_001", text_blob(rng, 140000))                # empacotado
    w(root, "d/gfx/bootload_main", text_blob(rng, 50000))               # solto (d/*/bootload*)
    w(root, "d/x/critbootload", text_blob(rng, 50000))                  # solto
    w(root, "d/snd/soundbank_0", text_blob(rng, 50000))                 # solto (d/*/soundbank*)
    w(root, "d/wem/wem_01", text_blob(rng, 50000))                      # solto (d/*/wem*)
    w(root, "d/movie/movie_01", text_blob(rng, 50000))                  # solto
    w(root, "other/extra.arc", text_blob(rng, 50000))                   # solto (default_action = loose)


def make_insomniac_title(root: Path) -> None:
    """Title ID do Wolverine sem o layout: o perfil vem do sce_sys/param.json."""
    rng = make_base(root, 34, "PPSA03671")
    w(root, "d/gfx/archive_000", text_blob(rng, 100000))
    w(root, "x/other.arc", text_blob(rng, 100000))


def make_unity_profile(root: Path) -> None:
    """Unity com StreamingAssets (perfil unity, por layout): só StreamingAssets/* empacotado."""
    rng = make_base(root, 66)
    w(root, "Media/Data/globalgamemanagers", text_blob(rng, 60000))
    w(root, "Media/Data/level0", text_blob(rng, 60000))
    w(root, "Media/Data/sharedassets0.assets", text_blob(rng, 90000))
    w(root, "Media/Data/sharedassets0.assets.resS", text_blob(rng, 90000))
    w(root, "Media/Data/Managed/Metadata/global-metadata.dat", text_blob(rng, 30000))
    w(root, "Media/StreamingAssets/aa/PS5/catalog_a.bundle", text_blob(rng, 200000))
    w(root, "Media/StreamingAssets/aa/PS5/catalog_b.bundle", text_blob(rng, 100000))


def make_playgo(root: Path, with_stub: bool) -> None:
    """Marcadores do PlayGo e boot do Ghost of Yōtei sempre no disco; eboot importa libScePlayGo."""
    rng = make_base(root, 11, extra=b"libScePlayGo.sprx\0")
    w(root, "cache_ps5/playgo.pgm", text_blob(rng, 40000))
    w(root, "cache_ps5/pgc_0_dummy_file", text_blob(rng, 20000))
    w(root, "cache_ps5/pgc_1_dummy_file", b"")
    w(root, "cache_ps5/game.sprig.packman", text_blob(rng, 40000))
    w(root, "sounds/scream/_generated/group_mix.gmx", text_blob(rng, 20000))
    w(root, "cache_ps5/assets.big", text_blob(rng, 200000))             # empacotado
    w(root, "Runtime/packagedefinition.txt", b"patchlevel=0\n")
    if with_stub:
        w(root, "fakelib/libScePlayGo.sprx", b"\x7fELF PlayGo PRX stub " + os.urandom(3000))


def estimate(out: str) -> tuple[str, int, int]:
    kv = estimate_kv(out)
    return kv["engine"], int(kv["packed"]), int(kv["loose"])


def estimate_kv(out: str) -> dict[str, str]:
    for line in out.splitlines():
        if line.startswith("estimativa: "):
            return dict(x.split("=", 1) for x in line[len("estimativa: "):].split())
    raise SystemExit("host_convert não imprimiu a estimativa:\n" + out)


def src_total(out: str) -> int:
    line = out.strip().splitlines()[-1]
    return int(line.split("origem=")[1].split()[0])


def profile_case(t: Path, name: str, maker, expect_engine: str, loose: list[str], packed: list[str],
                 keep: str | None = None, profile: str = "-", hot: list[str] = (), shifts: list[str] = (),
                 src_mode: int | None = None, extra: list[str] = ()) -> str:
    src = t / f"{name}-src"
    maker(src)
    if src_mode is not None:  # origem com permissões restritas (como uma imagem .exfat 0666/0644)
        for dp, _dn, fn in os.walk(src):
            for f in fn:
                os.chmod(Path(dp) / f, src_mode)
    dst = t / f"{name}-out"
    cmd = [str(CONV), "-l", "5", "-w", "2"] + (["-k", keep] if keep is not None else []) + list(extra) + [str(src), str(dst)]
    out = run(cmd)
    eng, pk, lo = estimate(out)
    assert eng == expect_engine, (name, eng, expect_engine)
    prof = estimate_kv(out)["profile"]
    assert prof == profile, (name, prof, profile)
    assert pk + lo == src_total(out), (name, pk, lo, src_total(out))
    args = [sys.executable, "-I", str(VALIDATE), str(src), str(dst)]
    for r in loose:
        args += ["--expect-loose", r]
    for r in packed:
        args += ["--expect-packed", r]
    for r in hot:
        args += ["--expect-hot", r]
    for r in shifts:
        args += ["--expect-block-shift", r]
    vout = run(args)
    assert "VALIDAÇÃO OK" in vout, vout
    assert "CFG1 ok: workers=8 reserva=1" in vout, vout
    print(f"perfil {name}: engine={eng} profile={prof} packed={pk} B loose={lo} B; "
          f"soltos {loose}, empacotados {packed}" + (f", HOT {list(hot)}" if hot else "")
          + (f", blocos {list(shifts)}" if shifts else "") + ": VALIDAÇÃO OK (0777, CFG1 8 workers)")
    return out


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


def make_incompressible(root: Path) -> None:
    """Arquivo >= 64 MiB incompressível (auto-loose) + um pequeno compressível."""
    rng = make_base(root, 11)
    w(root, "Data/huge_random.big", os.urandom(64 << 20))
    w(root, "Data/small.arc", text_blob(rng, 90000))


def unreal_cases(t: Path) -> None:
    profile_case(t, "unreal", make_unreal, "unreal",
                 [".utoc", ".pak", ".sig", "global.ucas", "global.utoc", "Game/Content/Paks/Sub/GLOBAL.UCAS",
                  "Engine/Config/", "Game/Config/", ".ini", ".uproject", ".uplugin", ".upluginmanifest",
                  ".locres", ".locmeta", "Engine/Content/Internationalization/"],
                 ["Game/Content/Paks/pakchunk0-ps5.ucas", "Game/Content/Paks/pakchunk1-ps5.ucas", ".uasset",
                  ".ubulk", "Game/Content/Movies/intro.dat2", "Game/Content/Data/blob.dat2"])
    # keep_loose do usuário ainda tira os .ucas dos pacotes (comportamento antigo, se precisar)
    profile_case(t, "keep-unreal", make_unreal, "unreal", [".ucas", "Game/Config/"],
                 ["Game/Content/Movies/intro.dat2", ".uasset"], keep=".ucas,Game/Config/")


def pack_all_cases(t: Path) -> None:
    size = 64 << 20
    out = profile_case(t, "autoloose", make_incompressible, "unknown", ["Data/huge_random.big"], ["Data/small.arc"])
    kv = estimate_kv(out)
    assert kv["pack_all"] == "0" and int(kv["auto_loose"]) == size, kv
    assert "auto-loose: 1 arquivo(s), 64 MiB soltos" in out, out
    out = profile_case(t, "pack-all", make_incompressible, "unknown", [], ["Data/huge_random.big", "Data/small.arc"],
                       extra=["--pack-all"])
    kv = estimate_kv(out)
    assert kv["pack_all"] == "1" and int(kv["auto_loose"]) == 0, kv
    assert "pack_all ativo" in out, out
    print("pack_all: arquivo incompressível de 64 MiB solto por padrão e empacotado com --pack-all")


def make_compressible(root: Path) -> None:
    """70 MiB muito compressíveis (vão para os pacotes) + um pequeno."""
    rng = make_base(root, 12)
    w(root, "Data/huge_text.big", text_blob(rng, 1 << 20) * 70)
    w(root, "Data/small.arc", text_blob(rng, 90000))


def space_cases(t: Path) -> None:
    """Pré-checagem de espaço: usa a estimativa por amostragem (AMPR_TEST_FREE_MIB, só no host)."""
    import re
    def conv(src: Path, dst: Path, free: int, ok: bool) -> str:
        env = dict(os.environ, AMPR_TEST_FREE_MIB=str(free))
        r = subprocess.run([str(CONV), str(src), str(dst)], capture_output=True, text=True, env=env)
        assert (r.returncode == 0) == ok, r.stdout + r.stderr
        return r.stdout + r.stderr
    comp, rnd = t / "sp-comp", t / "sp-rand"
    make_compressible(comp)
    make_incompressible(rnd)
    # compressível: estimado ~ margem (512 MiB) + pouco, bem abaixo do tamanho lógico (70 MiB + margem = 582)
    out = conv(comp, t / "sp-comp-fail", 400, False)
    need = int(re.search(r"necessário (\d+) MiB", out).group(1))
    assert "espaço insuficiente" in out and 512 <= need < 530, out
    assert not (t / "sp-comp-fail").exists(), "destino criado apesar do erro"
    out = conv(comp, t / "sp-comp-ok", 540, True)
    assert re.search(r"espaço: livre 540 MiB, estimado \d+ MiB \(amostra", out), out
    # incompressível >= 64 MiB: auto-loose conta pelo tamanho cheio (>= 64 + 512 MiB)
    out = conv(rnd, t / "sp-rand-fail", 560, False)
    need = int(re.search(r"necessário (\d+) MiB", out).group(1))
    assert need >= 576, out
    conv(rnd, t / "sp-rand-ok", 700, True)
    print("space: estimativa por amostragem ok (compressível < cheio; incompressível = cheio)")


def main() -> int:
    big = "--big" in sys.argv
    only = sys.argv[sys.argv.index("--only") + 1].split(",") if "--only" in sys.argv else None
    if only:  # rodar só alguns casos: --only unreal,pack_all
        cases = {"unreal": unreal_cases, "pack_all": pack_all_cases, "space": space_cases}
        with tempfile.TemporaryDirectory(prefix="ampr-e2e-") as td:
            for n in only:
                cases[n](Path(td))
        print("E2E (" + ",".join(only) + ") OK")
        return 0
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
        # perfis por engine e regras extras (keep_loose)
        unreal_cases(t)
        profile_case(t, "northlight", make_northlight, "northlight", [".rmdtoc"], [".rmdp"])
        profile_case(t, "unity", make_unity, "unity", [], [".assets", ".resS", "globalgamemanagers"])
        profile_case(t, "keep", lambda r: make_game(r, False), "unknown",
                     [".res", "Mixed/", "exact.bin2", "Data/Level1.ARC", "Data/tiny.cfg"], ["Data/random.arc"],
                     keep=" .RES , mixed/,EXACT.bin2,/app0/data/level1.arc")
        # sem regra, .res e Level1.ARC iriam para os pacotes (garante que o caso keep_loose testa algo)
        profile_case(t, "keep-none", lambda r: make_game(r, False), "unknown", [],
                     ["dup_a.res", "Data/Level1.ARC", "Mixed/"],
                     hot=["Sound/ui_bank.pck", "Data/soundbank.pck"], shifts=["Sound/ui_bank.pck=14", "Data/Level1.ARC=16"])
        # Insomniac (Wolverine): d/ comprimido em blocos de 128 KiB; boot, sound banks, wem, movie, toc/dag soltos
        profile_case(t, "insomniac", make_insomniac, "unknown",
                     ["toc", "dag", "d/gfx/bootload_main", "d/x/critbootload", "d/snd/soundbank_0", "d/wem/",
                      "d/movie/", "other/", "commandline.txt"],
                     ["d/gfx/archive_000", "d/gfx/archive_001"], profile="insomniac",
                     shifts=["d/gfx/archive_000=17"])
        out = profile_case(t, "insomniac-tid", make_insomniac_title, "unknown", ["x/other.arc"],
                           ["d/gfx/archive_000"], profile="insomniac")
        assert "perfil insomniac (por title-id; Title ID PPSA03671)" in out, out
        # Unity com perfil: só StreamingAssets empacotado; level*, sharedassets*, globalgamemanagers, .resS soltos
        profile_case(t, "unity-profile", make_unity_profile, "unity",
                     ["Media/Data/globalgamemanagers", "Media/Data/level0", "sharedassets0.assets",
                      "sharedassets0.assets.resS", "global-metadata.dat"],
                     ["Media/StreamingAssets/"], profile="unity")
        # PlayGo/Yōtei sempre no disco; aviso de libScePlayGo sem stub; origem 0644 vira 0777 no destino
        out = profile_case(t, "playgo", lambda r: make_playgo(r, False), "unknown",
                           ["cache_ps5/playgo.pgm", "cache_ps5/pgc_0_dummy_file", "cache_ps5/pgc_1_dummy_file",
                            "cache_ps5/game.sprig.packman", "sounds/scream/_generated/group_mix.gmx",
                            "Runtime/packagedefinition.txt"],
                           ["cache_ps5/assets.big"], src_mode=0o644)
        assert estimate_kv(out)["playgo_missing"] == "1" and "libScePlayGo" in out, out
        out = profile_case(t, "playgo-stub", lambda r: make_playgo(r, True), "unknown",
                           ["cache_ps5/playgo.pgm", "fakelib/libScePlayGo.sprx"], ["cache_ps5/assets.big"])
        assert estimate_kv(out)["playgo_missing"] == "0", out
        print("PlayGo: aviso sem fakelib/libScePlayGo.sprx; sem aviso com o stub na origem")
        pack_all_cases(t)
        space_cases(t)
    print("E2E OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
