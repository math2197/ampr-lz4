#!/usr/bin/env python3
"""Validador de uma pasta convertida para AMPR (GPL-3.0).

Usa as ferramentas de referência do ampr_emu (tests/ref, GPL-3.0) para ler/validar os formatos
e acrescenta as checagens do carregador C++ (ampr_emu_index.cpp / ampr_emu_pack_core.cpp) que o
Python não faz. Por fim descomprime todos os arquivos pelo manifesto e compara SHA-256 com a origem.

uso: python3 -I tests/validate_ampr.py <origem> <destino> [--fakelib caminho.sprx]
         [--expect-loose REGRA ...] [--expect-packed REGRA ...]
REGRA usa a mesma sintaxe de keep_loose: ".ucas" (extensão), "global.ucas" (nome) ou
"Content/Paks/" (prefixo de caminho relativo, com '/'). Cada regra precisa casar ao menos um arquivo,
e todos os que casam precisam estar soltos (ou empacotados) no destino.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import stat
import struct
import sys
import tempfile
from pathlib import Path

REF = Path(__file__).resolve().parent / "ref"
sys.path.insert(0, str(REF))

import ampr_pack  # noqa: E402  (tools/ampr_pack.py do ampr_emu)
import ampr_pack_format as fmt  # noqa: E402
import build_ampr_index as bai  # noqa: E402

EXEC_EXT = (".elf", ".self", ".prx", ".sprx")
OUTPUT_NAMES = {
    "ampr_emu.index",
    "ampr_assets.index",
    "ampr_assets.index.crc",
    "ampr_assets.index.runtime",
}


class Fail(Exception):
    pass


def check(cond: bool, msg: str) -> None:
    if not cond:
        raise Fail(msg)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            b = f.read(8 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def source_rows(src: Path, fakelib: Path | None) -> list[tuple[int, int, str]]:
    """Mesma coleta de build_index_local (build_ampr_index.py), sobre a ORIGEM.

    Diferenças deliberadas: o índice gerado mora no destino (não há arquivo de saída a excluir
    na origem) e ampr_emu.index* da origem também ficam de fora, como no caminho FTP do script."""
    rows: list[tuple[int, int, str]] = []
    seen: dict[bytes, str] = {}
    for dirpath, dirnames, filenames in os.walk(src):
        dirnames.sort(key=bai.key_for)
        filenames.sort(key=bai.key_for)
        for name in filenames:
            path = Path(dirpath) / name
            indexed = bai.app0_path(src, path)
            if bai.key_for(indexed) in {
                bai.key_for(x)
                for x in (
                    "/app0/ampr_commands.bin",
                    "/app0/apr_emu.log",
                    "/app0/ampr_emu.index",
                    "/app0/ampr_emu.index.tmp",
                    "/app0/ampr_emu.index.bak",
                )
            }:
                continue
            st = path.lstat()
            check(stat.S_ISREG(st.st_mode), f"origem contém não-regular: {indexed}")
            if fakelib is not None and bai.key_for(indexed) == bai.key_for("/app0/fakelib/libSceAmpr.sprx"):
                continue
            ok = bai.validate_and_add_row(rows, seen, st.st_size, int(st.st_mtime), indexed, False)
            check(ok, f"colisão de caminho na origem: {indexed}")
    if fakelib is not None:
        st = fakelib.stat()
        ok = bai.validate_and_add_row(rows, seen, st.st_size, int(st.st_mtime), "/app0/fakelib/libSceAmpr.sprx", False)
        check(ok, "colisão fakelib")
    return rows


def rule_match(rel: str, rule: str) -> bool:
    """Mesma semântica de scan_keep_loose_match (src/scan.c), para um único token."""
    t = rule.strip()
    if t.lower().startswith("/app0/"):
        t = t[6:]
    t = t.lstrip("/")
    if not t:
        return False
    r, tl, base = rel.lower(), t.lower(), rel.rsplit("/", 1)[-1].lower()
    if "/" in t:
        return r.startswith(tl) and (len(r) == len(tl) or r[len(tl)] == "/" or tl.endswith("/"))
    if t.startswith("."):
        return base.endswith(tl)
    return base == tl


def check_idx3_like_cpp(data: bytes) -> None:
    """Reproduz a carga de AMPRIDX3 do ampr_emu_index.cpp (contagens, tabela hash, buscas)."""
    hdr = struct.unpack_from("<8sIIQQQII", data)
    magic, version, entry_size, n, path_bytes, hash_off, slot_size, slots = hdr
    check(magic == b"AMPRIDX3" and version == 3 and entry_size == 24 and slot_size == 16, "IDX3 cabeçalho")
    check(0 < n <= 2_000_000 and 0 < path_bytes <= 256 << 20, "IDX3 contagens")
    check(slots >= 2 and slots & (slots - 1) == 0 and slots >= n, "IDX3 slots")
    path_off = 48 + n * 24
    min_hash = (path_off + path_bytes + 15) & ~15
    check(hash_off >= min_hash and hash_off % 16 == 0, "IDX3 hash_offset")
    check(hash_off + slots * 16 <= len(data), "IDX3 truncado")
    blob = data[path_off : path_off + path_bytes]
    paths = []
    prev = None
    for i in range(n):
        at, ln, _size, _mtime = struct.unpack_from("<IIQq", data, 48 + i * 24)
        check(at + ln < path_bytes and blob[at + ln] == 0, f"IDX3 caminho {i}")
        p = blob[at : at + ln]
        check(b"\0" not in p and p.startswith(b"/app0/") and len(p) > 6, f"IDX3 caminho {i} formato")
        k = bai.key_for(p.decode("utf-8"))
        if prev is not None:
            check(prev < k, f"IDX3 não ordenado/duplicado em {i}")
        prev = k
        paths.append(p.decode("utf-8"))
    occupied = 0
    table = []
    for s in range(slots):
        h, ipo, flags = struct.unpack_from("<QII", data, hash_off + s * 16)
        table.append((h, ipo, flags))
        if ipo == 0:
            check(h == 0 and flags == 0, f"IDX3 slot vazio sujo {s}")
            continue
        occupied += 1
        check(ipo <= n and flags & ~1 == 0, f"IDX3 slot {s}")
        check(h == bai.fnv1a64_path_hash(paths[ipo - 1]), f"IDX3 hash do slot {s} (base 1469598103934665603)")
    check(occupied == n, "IDX3 ocupação")
    mask = slots - 1
    for i, p in enumerate(paths):
        h = bai.fnv1a64_path_hash(p)
        pos = h & mask
        found = False
        for _ in range(slots):
            sh, ipo, _f = table[pos]
            if ipo == 0:
                break
            if sh == h and bai.key_for(paths[ipo - 1]) == bai.key_for(p):
                found = ipo == i + 1
                break
            pos = (pos + 1) & mask
        check(found, f"IDX3 busca falhou para {p}")


def check_manifest_like_cpp(m: fmt.PackManifest) -> None:
    """Checagens do ampr_emu_pack_core.cpp ausentes em validate_manifest."""
    for fid, rec in enumerate(m.files, 1):
        if not rec.flags & fmt.FILE_FLAG_PACKED:
            continue
        bs = 1 << rec.block_shift
        check(rec.chunk_count == (rec.logical_size + bs - 1) // bs, f"chunk_count esperado no arquivo {fid}")
        for local in range(rec.chunk_count):
            ch = m.chunks[rec.first_chunk + local]
            pk = m.packs[ch.pack_id]
            if not rec.flags & fmt.FILE_FLAG_STREAMING:
                if ch.stored_size <= pk.io_page_size:
                    check(bool(ch.flags & fmt.CHUNK_FLAG_PAGE_CONTAINED), f"chunk sem CONTAINED no arquivo {fid}")
                else:
                    check(bool(ch.flags & fmt.CHUNK_FLAG_PAGE_ALIGNED), f"chunk sem ALIGNED no arquivo {fid}")
                if ch.codec == fmt.CHUNK_CODEC_RAW and ch.stored_size >= pk.io_page_size:
                    check(bool(ch.flags & fmt.CHUNK_FLAG_PAGE_ALIGNED), f"RAW >= página desalinhado no arquivo {fid}")


def check_volume_gaps(dst: Path, m: fmt.PackManifest) -> int:
    """Bytes fora de cabeçalho e chunks devem ser zero. Retorna bytes de padding."""
    padding = 0
    ranges: dict[int, set[tuple[int, int]]] = {}
    for ch in m.chunks:
        ranges.setdefault(ch.pack_id, set()).add((ch.offset, ch.offset + ch.stored_size))
    for pid, rec in enumerate(m.packs):
        data = (dst / m.pack_name(pid)).read_bytes()
        check(len(data) == rec.file_size, f"tamanho do volume {pid}")
        pos = fmt.DATA_HEADER_SIZE
        for a, b in sorted(ranges.get(pid, ())):
            check(a >= pos, f"chunks sobrepostos no volume {pid} em 0x{a:x}")
            check(data[pos:a].count(0) == a - pos, f"lacuna não-zero no volume {pid} em 0x{pos:x}")
            padding += a - pos
            pos = b
        check(data[pos:].count(0) == len(data) - pos, f"cauda não-zero no volume {pid}")
        padding += len(data) - pos
    return padding


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("src", type=Path)
    ap.add_argument("dst", type=Path)
    ap.add_argument("--fakelib", type=Path)
    ap.add_argument("--expect-loose", action="append", default=[], metavar="REGRA")
    ap.add_argument("--expect-packed", action="append", default=[], metavar="REGRA")
    a = ap.parse_args()
    src, dst = a.src.resolve(), a.dst.resolve()

    # 1) AMPRIDX3: byte a byte igual ao gerado por build_ampr_index.write_index sobre a origem
    idx_path = dst / "ampr_emu.index"
    rows = source_rows(src, a.fakelib)
    with tempfile.TemporaryDirectory() as td:
        ref_idx = Path(td) / "ampr_emu.index"
        bai.write_index(rows, ref_idx)
        ref_bytes = ref_idx.read_bytes()
    ours = idx_path.read_bytes()
    check(ours == ref_bytes, "ampr_emu.index difere do gerado por build_ampr_index.py")
    check_idx3_like_cpp(ours)
    entries = fmt.read_ampridx3(idx_path)
    print(f"IDX3 ok: {len(entries)} entradas, idêntico ao build_ampr_index.py, tabela hash validada")

    # 2) AMPRPAK4 + CRC1 + CFG1 + AMPRDAT3 pelas ferramentas de referência
    man_path = dst / "ampr_assets.index"
    m = fmt.load_manifest(man_path)  # inclui validate_manifest
    check_manifest_like_cpp(m)
    check(len(m.files) == len(entries), "PAK4 e IDX3 com contagens diferentes")
    for fid, (e, rec) in enumerate(zip(entries, m.files), 1):
        check(m.file_path(fid) == e.path, f"fileId {fid}: caminho difere ({m.file_path(fid)} != {e.path})")
        check(rec.logical_size == e.size and rec.mtime == e.mtime, f"fileId {fid}: tamanho/mtime difere")
        check(rec.path_hash == fmt.asset_path_hash(e.path), f"fileId {fid}: path_hash (base 0xCBF29CE484222325)")
    crcs = fmt.load_chunk_crcs(fmt.chunk_crc_path(man_path), m.build_id, len(m.chunks))
    rt = fmt.read_runtime_settings(Path(str(man_path) + ".runtime"), m.build_id)
    check(rt is not None and rt == fmt.RuntimeSettings(128 << 20, 32 << 20, 4, 1), f"runtime: {rt}")
    vstats = ampr_pack.verify_packs(man_path)
    rstats = ampr_pack.verify_packs_against_root(man_path, src)
    padding = check_volume_gaps(dst, m)
    print(f"PAK4 ok: build_id={m.build_id.hex()} arquivos={len(m.files)} chunks={len(m.chunks)} "
          f"volumes={len(m.packs)} crcs={len(crcs)}")
    print(f"verify_packs (ref): {vstats}")
    print(f"verify_packs_against_root (ref): {rstats}")

    # 3) SHA-256 de todos os arquivos: empacotados (descomprimidos pelo manifesto) e loose
    expected_on_disk: set[str] = set(OUTPUT_NAMES)
    expected_on_disk.update(m.pack_name(i) for i in range(len(m.packs)))
    packed = loose = 0
    is_packed: dict[str, bool] = {}
    packed_bytes = loose_bytes = 0
    n_lz4 = sum(1 for c in m.chunks if c.codec == fmt.CHUNK_CODEC_LZ4)
    n_shared = sum(1 for c in m.chunks if c.flags & fmt.CHUNK_FLAG_SHARED)
    with ampr_pack.PackReader(m) as reader:
        for fid, rec in enumerate(m.files, 1):
            rel = fmt.asset_relative_path(m.file_path(fid))
            if a.fakelib is not None and rel.lower() == "fakelib/libsceampr.sprx":
                srcf = a.fakelib
            else:
                srcf = src / rel
            want = sha256_file(srcf)
            is_packed[rel] = bool(rec.flags & fmt.FILE_FLAG_PACKED)
            if rec.flags & fmt.FILE_FLAG_PACKED:
                h = hashlib.sha256()
                for raw in reader.iter_file_data(fid):
                    h.update(raw)
                check(h.hexdigest() == want, f"SHA-256 difere (empacotado): {rel}")
                check(not (dst / rel).exists(), f"arquivo empacotado também copiado: {rel}")
                packed += 1
                packed_bytes += rec.logical_size
            else:
                d = dst / rel
                check(d.is_file(), f"loose ausente: {rel}")
                check(sha256_file(d) == want, f"SHA-256 difere (loose): {rel}")
                base = rel.rsplit("/", 1)[-1].lower()
                is_exec = base.endswith(EXEC_EXT) or base == "eboot.bin"
                mode = stat.S_IMODE(d.stat().st_mode)
                check(mode == (0o755 if is_exec else 0o644), f"permissão {oct(mode)} em {rel}")
                expected_on_disk.add(rel)
                loose += 1
                loose_bytes += rec.logical_size

    # 4) nada sobrando/faltando no destino; diretórios da origem recriados
    actual: set[str] = set()
    for dirpath, dirnames, filenames in os.walk(dst):
        for name in filenames:
            actual.add((Path(dirpath) / name).relative_to(dst).as_posix())
    extra = actual - expected_on_disk
    missing = expected_on_disk - actual
    check(not extra, f"arquivos inesperados no destino: {sorted(extra)[:10]}")
    check(not missing, f"arquivos faltando no destino: {sorted(missing)[:10]}")
    for dirpath, dirnames, _ in os.walk(src):
        for dn in dirnames:
            rel = (Path(dirpath) / dn).relative_to(src)
            check((dst / rel).is_dir(), f"diretório não recriado: {rel}")

    # 5) regras esperadas: arquivos que precisam ficar soltos (lidos fora da API AMPR) ou empacotados
    for rules, want_packed, label in ((a.expect_loose, False, "solto"), (a.expect_packed, True, "empacotado")):
        for rule in rules:
            hits = [r for r in is_packed if rule_match(r, rule)]
            check(bool(hits), f"regra {rule!r} não casou nenhum arquivo")
            bad = [r for r in hits if is_packed[r] != want_packed]
            check(not bad, f"regra {rule!r}: esperado {label}, mas não está: {sorted(bad)[:10]}")
            if not want_packed:
                for r in hits:
                    check((dst / r).is_file(), f"regra {rule!r}: {r} não está solto no destino")
            print(f"regra {label} ok: {rule!r} ({len(hits)} arquivo(s))")

    src_total = sum(r[0] for r in rows)
    dst_total = sum((dst / p).stat().st_size for p in actual)
    print(f"SHA-256 ok: {packed} empacotados ({packed_bytes} B), {loose} loose ({loose_bytes} B)")
    print(f"chunks: lz4={n_lz4} raw={len(m.chunks) - n_lz4} compartilhados={n_shared} padding={padding} B")
    print(f"origem={src_total} B destino={dst_total} B razão={100.0 * dst_total / max(1, src_total):.2f}%")
    print("VALIDAÇÃO OK")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (Fail, ValueError, ampr_pack.PackError) as exc:
        print(f"VALIDAÇÃO FALHOU: {exc}", file=sys.stderr)
        raise SystemExit(1)
