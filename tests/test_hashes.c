/* Testes unitários: FNV-1a 64 (bases PAK4 e IDX3), CRC32 (zlib), SHA-256, decisão de codec. GPL-3.0.
 * Valores esperados de FNV gerados com tests/ref/ampr_pack_format.py (asset_path_hash) e
 * tests/ref/build_ampr_index.py (fnv1a64_path_hash). */
#include "../src/ampr_pack.h"
#include "../src/util.h"
#include "../src/profile.h"
#include "../src/scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, total;
#define CHECK(cond, ...)                         \
  do {                                           \
    total++;                                     \
    if (!(cond)) {                               \
      fails++;                                   \
      printf("FALHA %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                       \
      printf("\n");                              \
    }                                            \
  } while (0)

static uint32_t crc_bitwise(const uint8_t *p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
  }
  return c ^ 0xFFFFFFFFu;
}

static void hex(const uint8_t *d, size_t n, char *out) {
  for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

static void test_fnv(void) {
  struct { const char *p; uint64_t pak4, idx3; } v[] = {
      {"/app0/eboot.bin", 0x5c2073b8583bcf34ull, 0xa8641f238cf4f1aeull},
      {"/APP0/Data\\Level1.PAK", 0x7f434ffcae1b5b9eull, 0x45b7f5eeedf69230ull},
      {"/app0/sce_sys/param.json", 0x2501679e4f1906dbull, 0x5dcdce007c4597f9ull},
      {"/app0/\xc3\xa7\xc3\x83o/\xc3\x81.dat", 0x27b36d0bc7591f25ull, 0x36a26a18f026f413ull},
  };
  for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
    uint64_t a = ampr_path_hash(v[i].p, strlen(v[i].p), FNV64_BASIS_PAK4);
    uint64_t b = ampr_path_hash(v[i].p, strlen(v[i].p), FNV64_BASIS_IDX3);
    CHECK(a == v[i].pak4, "PAK4 %s: %016llx != %016llx", v[i].p, (unsigned long long)a, (unsigned long long)v[i].pak4);
    CHECK(b == v[i].idx3, "IDX3 %s: %016llx != %016llx", v[i].p, (unsigned long long)b, (unsigned long long)v[i].idx3);
  }
  CHECK(FNV64_BASIS_PAK4 != FNV64_BASIS_IDX3, "bases devem diferir");
  CHECK(FNV64_BASIS_IDX3 == 0x14650FB0739D0383ull, "base IDX3 = 1469598103934665603");
  CHECK(ampr_path_hash("", 0, FNV64_BASIS_PAK4) == 0xCBF29CE484222325ull, "hash vazio = base");
  char s[] = "/APP0/A\\B.Txt";
  ampr_fold_path(s, strlen(s));
  CHECK(strcmp(s, "/app0/a/b.txt") == 0, "dobra: %s", s);
}

static void test_crc(void) {
  CHECK(crc32_update(0, "123456789", 9) == 0xCBF43926u, "crc check value");
  CHECK(crc32_update(0, "", 0) == 0, "crc vazio");
  CHECK(crc32_update(0, "The quick brown fox jumps over the lazy dog", 43) == 0x414FA339u, "crc fox");
  /* incremental == de uma vez; todos os alinhamentos/tamanhos */
  uint8_t *buf = malloc(5000);
  uint32_t x = 12345;
  for (int i = 0; i < 5000; i++) { x = x * 1103515245u + 12345u; buf[i] = (uint8_t)(x >> 16); }
  for (size_t off = 0; off < 9; off++)
    for (size_t n = 0; n < 300; n += 7)
      CHECK(crc32_update(0, buf + off, n) == crc_bitwise(buf + off, n), "crc off=%zu n=%zu", off, n);
  uint32_t inc = crc32_update(0, buf, 1234);
  inc = crc32_update(inc, buf + 1234, 5000 - 1234);
  CHECK(inc == crc_bitwise(buf, 5000), "crc incremental");
  free(buf);
}

static void test_sha(void) {
  char h[65];
  uint8_t d[32];
  sha256("", 0, d); hex(d, 32, h);
  CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "sha vazio %s", h);
  sha256("abc", 3, d); hex(d, 32, h);
  CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha abc %s", h);
  const char *m2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  sha256(m2, strlen(m2), d); hex(d, 32, h);
  CHECK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "sha 2 blocos %s", h);
  sha256_ctx c;
  sha256_init(&c);
  char a[1000];
  memset(a, 'a', sizeof a);
  for (int i = 0; i < 1000; i++) sha256_update(&c, a, 1000);
  sha256_final(&c, d); hex(d, 32, h);
  CHECK(!strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "sha 1M 'a' %s", h);
  /* fronteira 55/56/64 bytes, incremental byte a byte */
  for (int n = 50; n < 70; n++) {
    uint8_t d1[32], d2[32];
    sha256(a, (size_t)n, d1);
    sha256_init(&c);
    for (int i = 0; i < n; i++) sha256_update(&c, a + i, 1);
    sha256_final(&c, d2);
    CHECK(!memcmp(d1, d2, 32), "sha incremental n=%d", n);
  }
}

static void test_keep(void) {
  /* _compress_block (layout mixed): blocos <= 1 página exigem >= 8 KiB e >= 12,5 % */
  CHECK(ampr_keep_lz4(65536, 57344, 65536) == 1, "64K->56K mantém (12,5%% exato)");
  CHECK(ampr_keep_lz4(65536, 57345, 65536) == 0, "64K->56K+1 RAW");
  CHECK(ampr_keep_lz4(16384, 8192, 65536) == 1, "16K->8K mantém");
  CHECK(ampr_keep_lz4(16384, 8193, 65536) == 0, "16K->8K+1 RAW (8 KiB mínimo)");
  CHECK(ampr_keep_lz4(100, 0, 65536) == 0, "falha de compressão RAW");
  CHECK(ampr_keep_lz4(131072, 65536, 65536) == 1, "128K->64K economiza página");
  CHECK(ampr_keep_lz4(131072, 131072 - 1311, 65536) == 0, "128K, <1%% RAW");
}

static void test_keep_ex(void) {
  /* regras de áudio random/hot: 16 B / 0,25 %, io_neutral 2 KiB / 12,5 % */
  CHECK(ampr_keep_lz4_ex(16384, 14336, 65536, 16, 25, 2048, 1250) == 1, "áudio 16K->14K mantém (2 KiB, 12,5%%)");
  CHECK(ampr_keep_lz4_ex(16384, 14337, 65536, 16, 25, 2048, 1250) == 0, "áudio 16K->14K+1 RAW");
  CHECK(ampr_keep_lz4(16384, 14336, 65536) == 0, "padrão 16K->14K RAW (8 KiB mínimo)");
}

static void test_runtime(void) {
  uint8_t rt[AMPR_CFG1_SIZE], id[16] = {1, 2, 3};
  ampr_runtime_encode(rt, id, 1000, 2000);
  uint64_t dec = le64_get(rt + 32), phy = le64_get(rt + 40);
  CHECK(!memcmp(rt, "AMPRCFG1", 8) && le32_get(rt + 48) == 8 && le32_get(rt + 52) == 1, "CFG1 8 workers, reserva 1");
  CHECK(phy == (64ull << 20), "CFG1 físico 64 MiB (%llu)", (unsigned long long)phy);
  /* 384 - (64+8+32+16) MiB - 3000 B - 64 MiB, alinhado a 16 KiB */
  uint64_t want = ((384ull << 20) - (120ull << 20) - 3000 - (64ull << 20)) & ~16383ull;
  CHECK(dec == want, "CFG1 decodificado %llu != %llu", (unsigned long long)dec, (unsigned long long)want);
  ampr_runtime_encode(rt, id, 400ull << 20, 0);
  CHECK(le64_get(rt + 32) == 0 && le64_get(rt + 40) == 0, "CFG1 sem orçamento: caches 0");
}

static void test_glob(void) {
  CHECK(prof_glob("d/*", "d/a/b.x"), "'*' casa '/'");
  CHECK(prof_glob("**/soundbank.*", "a/b/SoundBank.pck"), "** e maiúsculas");
  CHECK(!prof_glob("**/sound/**", "sound/x"), "**/x/** exige pasta pai (como fnmatch)");
  CHECK(prof_glob("cache_ps5/pgc_*_dummy_file", "cache_ps5/pgc_12_dummy_file"), "pgc dummy");
  CHECK(prof_glob("a?c", "abc") && !prof_glob("a?c", "ac"), "'?'");
  CHECK(!prof_glob("toc", "toc2") && prof_glob("toc", "TOC"), "nome exato");
}

static void cls(const char *rel, uint64_t size, int want_act, int want_locked, const char *what) {
  scan_file_t f;
  memset(&f, 0, sizeof f);
  scan_classify(rel, size, &f);
  CHECK(f.action == want_act && f.locked == want_locked, "%s: %s act=%d locked=%d (%s)", what, rel, f.action,
        f.locked, f.reason);
}

static void test_classify(void) {
  cls("Engine/Config/BaseEngine.ini", 10, SCAN_ACT_LOOSE, 1, ".ini");
  cls("Game/notes.TXT", 10, SCAN_ACT_LOOSE, 1, ".txt");
  cls("cache_ps5/playgo.pgm", 10, SCAN_ACT_LOOSE, 1, "playgo.pgm");
  cls("cache_ps5/pgc_3_dummy_file", 10, SCAN_ACT_LOOSE, 1, "pgc dummy");
  cls("sounds/scream/_generated/group_mix.gmx", 10, SCAN_ACT_LOOSE, 1, "Yotei");
  cls("Runtime/packagedefinition.txt", 10, SCAN_ACT_LOOSE, 1, "Glacier");
  cls("a/b/sce_sys/x.foo", 10, SCAN_ACT_LOOSE, 1, "sce_sys profundo");
  cls("data/save/slot.foo", 10, SCAN_ACT_LOOSE, 1, "save");
  cls("x/Mods/m.foo", 10, SCAN_ACT_LOOSE, 1, "mods");
  cls("x/trophy2/t.foo", 10, SCAN_ACT_LOOSE, 1, "trophy2");
  cls("x/uds/u.foo", 10, SCAN_ACT_LOOSE, 1, "uds");
  cls("decrypted/a/b.foo", 10, SCAN_ACT_LOOSE, 1, "decrypted");
  cls("x/tex.KTX2", 10, SCAN_ACT_LOOSE, 1, ".ktx2");
  cls("x/a.lz4", 10, SCAN_ACT_LOOSE, 1, ".lz4");
  cls("x/sub/eboot.bin", 10, SCAN_ACT_LOOSE, 1, "eboot profundo");
  cls("x/empty.foo", 0, SCAN_ACT_LOOSE, 1, "vazio");
  cls("x/data.foo", 10, SCAN_ACT_PACK, 0, "padrão");
  cls("rootfile", 10, SCAN_ACT_LOOSE, 0, "raiz (padrão, não travado)");
  cls("d/movie_hi/a.foo", 10, SCAN_ACT_LOOSE, 0, "contêiner de streaming");
  scan_file_t f;
  memset(&f, 0, sizeof f);
  scan_classify("data/Audio/sfx.foo", 100, &f);
  CHECK(f.action == SCAN_ACT_PACK && f.block_shift == 14 &&
            f.file_flags == (AMPR_FILE_FLAG_HOT | AMPR_FILE_FLAG_RANDOM_ACCESS) && f.sav_io_min_bytes == 2048,
        "áudio HOT|RANDOM_ACCESS 16 KiB");
  memset(&f, 0, sizeof f);
  scan_classify("data/soundbank.pck", 100, &f);
  CHECK(f.action == SCAN_ACT_PACK && f.file_flags == (AMPR_FILE_FLAG_HOT | AMPR_FILE_FLAG_RANDOM_ACCESS),
        "soundbank.* HOT|RANDOM_ACCESS");
  CHECK(!(f.file_flags & AMPR_FILE_FLAG_STREAMING), "nunca STREAMING junto de RANDOM_ACCESS");
}

static const prof_rule_t *sel(const prof_t *p, const char *rel) { return prof_select(p, rel); }

static void test_profiles(void) {
  char err[256];
  CHECK(prof_bundled_count() == 4, "4 perfis embutidos (%zu)", prof_bundled_count());
  for (size_t i = 0; i < prof_bundled_count(); i++) {
    prof_t p;
    int rc = prof_load_bundled(prof_bundled_name(i), &p, err, sizeof err);
    CHECK(rc == 0, "perfil %s: %s", prof_bundled_name(i), err);
    if (rc == 0) {
      CHECK(p.n_rules > 0 && p.def.action == PROF_ACT_LOOSE, "%s: regras e default loose", prof_bundled_name(i));
      for (size_t k = 0; k < p.n_rules; k++)
        CHECK((p.rules[k].file_flags & (AMPR_FILE_FLAG_STREAMING | AMPR_FILE_FLAG_RANDOM_ACCESS)) !=
                  (AMPR_FILE_FLAG_STREAMING | AMPR_FILE_FLAG_RANDOM_ACCESS),
              "%s regra %zu: STREAMING+RANDOM", prof_bundled_name(i), k);
      prof_free(&p);
    }
  }
  prof_t p;
  CHECK(prof_choose_bundled("PPSA03671", "", "", &p, err, sizeof err) == 1 && !strcmp(p.id, "insomniac") &&
            !strcmp(p.match, "title-id"),
        "Title ID -> insomniac");
  if (p.id[0]) {
    const prof_rule_t *r = sel(&p, "d/a/b.arc");
    CHECK(r->action == PROF_ACT_PACK && r->block_shift == 17, "insomniac d/* comprime 128 KiB");
    CHECK(sel(&p, "d/x/bootload_0")->action == PROF_ACT_LOOSE, "insomniac bootload solto");
    CHECK(sel(&p, "toc")->action == PROF_ACT_LOOSE && sel(&p, "dag") == &p.rules[1], "insomniac toc/dag soltos");
    CHECK(sel(&p, "other/file") == &p.def, "insomniac padrão");
    prof_free(&p);
  }
  CHECK(prof_choose_bundled("", "MARVEL'S Wolverine", "", &p, err, sizeof err) == 1 && !strcmp(p.match, "name"),
        "nome -> insomniac");
  prof_free(&p);
  CHECK(prof_choose_bundled("PPSA99999", "Outro", "unity", &p, err, sizeof err) == 1 && !strcmp(p.id, "unity") &&
            !strcmp(p.match, "layout"),
        "layout -> unity");
  if (p.id[0]) {
    CHECK(sel(&p, "Media/StreamingAssets/aa/x.bundle")->action == PROF_ACT_PACK, "unity StreamingAssets comprime");
    CHECK(sel(&p, "Media/Data/sharedassets0.resS")->action == PROF_ACT_LOOSE, "unity .resS solto");
    CHECK(sel(&p, "Media/Data/Managed/Metadata/global-metadata.dat")->action == PROF_ACT_LOOSE, "global-metadata");
    prof_free(&p);
  }
  CHECK(prof_choose_bundled("PPSA21567", "", "", &p, err, sizeof err) == 1 && !strcmp(p.id, "astrobot"), "astrobot");
  if (p.id[0]) {
    const prof_rule_t *r = sel(&p, "data/prein/sound/x.bnk2");
    CHECK(r->action == PROF_ACT_PACK && r->block_shift == 14 &&
              r->file_flags == (AMPR_FILE_FLAG_HOT | AMPR_FILE_FLAG_RANDOM_ACCESS) && r->sav.io_min_bytes == 2048 &&
              r->sav.min_bp == 25,
          "astrobot som: hot (layout auto -> random), 16 KiB, limiares de áudio");
    CHECK(p.unknown_keys == 1, "astrobot: 'ayout' ignorado (%d)", p.unknown_keys);
    prof_free(&p);
  }
  CHECK(prof_choose_bundled("PPSA00000", "Jogo X", "", &p, err, sizeof err) == 0, "sem perfil");
  const char *bad = "[pack]\ndefault_action = \"loose\"\n[[rule]]\naction = \"compress\"\nblock_size = \"3KiB\"\n"
                    "include = [\"a\"]\n";
  CHECK(prof_parse_toml(bad, strlen(bad), &p, err, sizeof err) != 0, "block_size inválido rejeitado");
  const char *ok = "# c\n[pack]\ndefault_action = 'compress' # c\ndefault_block_size = 32768\n[[rule]]\n"
                   "action = \"loose\"\ninclude = [\n  \"x/*\", # c\n  \"y\",\n]\nexclude = [\"x/keep\"]\n";
  CHECK(prof_parse_toml(ok, strlen(ok), &p, err, sizeof err) == 0, "TOML válido: %s", err);
  if (!err[0] || p.n_rules) {
    CHECK(p.def.action == PROF_ACT_PACK && p.def.block_shift == 15 && p.rules[0].n_include == 2, "TOML campos");
    CHECK(sel(&p, "x/a") == &p.rules[0] && sel(&p, "x/keep") == &p.def, "exclude");
    prof_free(&p);
  }
}

int main(void) {
  test_fnv();
  test_crc();
  test_sha();
  test_keep();
  test_keep_ex();
  test_runtime();
  test_glob();
  test_classify();
  test_profiles();
  printf("%d/%d verificações ok\n", total - fails, total);
  return fails ? 1 : 0;
}
