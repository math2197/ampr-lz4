/* Testes unitários: FNV-1a 64 (bases PAK4 e IDX3), CRC32 (zlib), SHA-256, decisão de codec. GPL-3.0.
 * Valores esperados de FNV gerados com tests/ref/ampr_pack_format.py (asset_path_hash) e
 * tests/ref/build_ampr_index.py (fnv1a64_path_hash). */
#include "../src/ampr_pack.h"
#include "../src/util.h"

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

int main(void) {
  test_fnv();
  test_crc();
  test_sha();
  test_keep();
  printf("%d/%d verificações ok\n", total - fails, total);
  return fails ? 1 : 0;
}
