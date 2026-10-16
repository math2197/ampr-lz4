/* Gerador do AMPRIDX3 (ampr_emu.index), espelhando tools/build_ampr_index.py do ampr_emu. GPL-3.0. */
#include "ampr_index.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ampr_idx3_build(const scan_result_t *r, uint8_t **out, size_t *out_len, char *err, size_t errlen) {
  *out = NULL;
  *out_len = 0;
  uint64_t n = r->n_indexed;
  if (n == 0 || n > 0xFFFFFFFEull) { snprintf(err, errlen, "AMPRIDX3: número de entradas inválido"); return -1; }
  uint64_t path_bytes = 0;
  for (size_t i = 0; i < r->n_files; i++)
    if (r->files[i].indexed) path_bytes += (uint64_t)r->files[i].app0_len + 1;
  if (path_bytes > 0xFFFFFFFFull) { snprintf(err, errlen, "AMPRIDX3: blob de caminhos grande demais"); return -1; }
  uint64_t slots = 2;
  while (slots < n * 2) slots <<= 1;
  if (slots > 0xFFFFFFFFull) { snprintf(err, errlen, "AMPRIDX3: tabela hash grande demais"); return -1; }
  uint64_t path_off = AMPR_IDX3_HEADER_SIZE + n * AMPR_IDX3_ENTRY_SIZE;
  uint64_t hash_off = align_up_u64(path_off + path_bytes, AMPR_IDX3_SLOT_SIZE);
  uint64_t total = hash_off + slots * AMPR_IDX3_SLOT_SIZE;
  uint8_t *b = (uint8_t *)calloc(1, (size_t)total);
  if (!b) { snprintf(err, errlen, "AMPRIDX3: sem memória"); return -1; }

  memcpy(b, "AMPRIDX3", 8);
  le32_put(b + 8, 3);
  le32_put(b + 12, AMPR_IDX3_ENTRY_SIZE);
  le64_put(b + 16, n);
  le64_put(b + 24, path_bytes);
  le64_put(b + 32, hash_off);
  le32_put(b + 40, AMPR_IDX3_SLOT_SIZE);
  le32_put(b + 44, (uint32_t)slots);

  uint64_t mask = slots - 1;
  uint8_t *slot_base = b + hash_off;
  uint64_t idx = 0, pat = 0;
  for (size_t i = 0; i < r->n_files; i++) {
    const scan_file_t *f = &r->files[i];
    if (!f->indexed) continue;
    uint8_t *e = b + AMPR_IDX3_HEADER_SIZE + idx * AMPR_IDX3_ENTRY_SIZE;
    le32_put(e, (uint32_t)pat);
    le32_put(e + 4, f->app0_len);
    le64_put(e + 8, f->size);
    le64_put(e + 16, (uint64_t)f->mtime);
    memcpy(b + path_off + pat, f->app0, f->app0_len); /* NUL já zerado pelo calloc */
    pat += f->app0_len + 1;

    /* Inserção com sondagem linear; hash repetido marca flag 1 nos dois slots. */
    uint64_t h = ampr_path_hash(f->app0, f->app0_len, FNV64_BASIS_IDX3);
    uint64_t pos = h & mask;
    int dup = 0;
    for (;;) {
      uint8_t *s = slot_base + pos * AMPR_IDX3_SLOT_SIZE;
      if (le32_get(s + 8) == 0) break;
      if (le64_get(s) == h) { le32_put(s + 12, le32_get(s + 12) | 1u); dup = 1; }
      pos = (pos + 1) & mask;
    }
    uint8_t *s = slot_base + pos * AMPR_IDX3_SLOT_SIZE;
    le64_put(s, h);
    le32_put(s + 8, (uint32_t)(idx + 1));
    le32_put(s + 12, dup ? 1u : 0u);
    idx++;
  }
  *out = b;
  *out_len = (size_t)total;
  return 0;
}
