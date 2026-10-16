/* Empacotador AMPRPAK4/AMPRDAT3/AMPRCRC1/AMPRCFG1 (fonte normativa: ampr_emu tools/ampr_pack*.py). GPL-3.0. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "convert.h"
#include "scan.h"

#define AMPR_PAK4_NAME "ampr_assets.index"
#define AMPR_CRC1_NAME "ampr_assets.index.crc"
#define AMPR_CFG1_NAME "ampr_assets.index.runtime"
#define AMPR_PACK_NAME_FMT "ampr_assets-%03u.pak"

#define AMPR_PAK4_HEADER_SIZE 128
#define AMPR_PAK4_FILE_REC 48
#define AMPR_PAK4_CHUNK_REC 12
#define AMPR_PAK4_PACK_REC 32
#define AMPR_DAT3_HEADER_SIZE 64
#define AMPR_CRC1_HEADER_SIZE 48
#define AMPR_CFG1_SIZE 64

#define AMPR_FILE_FLAG_PACKED 1u
#define AMPR_FILE_FLAG_STREAMING (1u << 2)     /* não gravado (só layout mixed); STREAMING e RANDOM_ACCESS são exclusivos */
#define AMPR_FILE_FLAG_HOT (1u << 3)
#define AMPR_FILE_FLAG_RANDOM_ACCESS (1u << 4)
#define AMPR_CHUNK_CODEC_RAW 0u
#define AMPR_CHUNK_CODEC_LZ4 1u
#define AMPR_CHUNK_FLAG_SHARED (1u << 0)
#define AMPR_CHUNK_FLAG_STREAMING (1u << 1)
#define AMPR_CHUNK_FLAG_PAGE_CONTAINED (1u << 2)
#define AMPR_CHUNK_FLAG_PAGE_ALIGNED (1u << 3)
#define AMPR_PACK_FLAG_IO_PAGE_LAYOUT (1u << 1)
#define AMPR_CHUNK_ALIGN 64u
#define AMPR_IO_PAGE_SIZE 65536u           /* f_iosize medido em nullfs/bfs/exfatfs (ASSET_PACKS.md) */
#define AMPR_MAX_VOLUME_BYTES (16ull << 30) /* rollover de volume */

typedef struct {
  const char *dst_dir;     /* volumes são gravados como <dst>/<nome>.tmp */
  int level;               /* 1..12 */
  int workers;             /* 1..16 */
  int dedup;               /* deduplicação de blocos idênticos (SHA-256/16 + tamanho + codec) */
  uint64_t max_volume;     /* 0 = AMPR_MAX_VOLUME_BYTES */
  conv_progress_t *p;      /* progresso/cancelamento (pode ser NULL) */
  int pack_all;            /* 1 = sem auto-loose (conv_opts_t.pack_all) */
} ampr_pack_opts_t;

typedef struct {
  uint8_t build_id[16];
  uint8_t *manifest;       /* bytes completos do ampr_assets.index */
  size_t manifest_len;
  uint8_t *crc;            /* bytes completos do .crc */
  size_t crc_len;
  uint8_t runtime[AMPR_CFG1_SIZE];
  uint32_t n_packs;        /* volumes gravados como <dst>/ampr_assets-NNN.pak.tmp */
  uint64_t volume_bytes;   /* soma dos tamanhos dos volumes */
  uint64_t logical_bytes, stored_bytes, padding_bytes;
  uint64_t chunks, chunks_lz4, chunks_raw, chunks_shared;
  uint64_t files_packed, files_loose, files_auto_loose;
  uint64_t auto_loose_bytes; /* bytes lógicos dos arquivos tirados dos pacotes pelo auto-loose */
} ampr_pack_result_t;

/* Decide auto-loose (amostragem), comprime e grava os volumes .tmp e monta manifesto/CRC/runtime em
 * memória. Pode trocar files[i].action de PACK para LOOSE (auto-loose / não indexados).
 * Retorna 0 ok, -1 erro (err), -2 cancelado. Em erro, os .tmp ficam no destino (o chamador remove). */
int ampr_pack_build(scan_result_t *r, const ampr_pack_opts_t *o, ampr_pack_result_t *res, char *err,
                    size_t errlen);
void ampr_pack_result_free(ampr_pack_result_t *res);

/* Estimativa de espaço de saída por amostragem (mesmo compressor/limiares do empacotador). */
typedef struct {
  uint64_t loose_bytes;      /* copiados soltos (inclui os que o auto-loose tiraria) */
  uint64_t auto_loose_bytes; /* parte de loose_bytes prevista pelo auto-loose */
  uint64_t packed_src_bytes; /* bytes lógicos planejados para os pacotes (antes do auto-loose) */
  uint64_t packed_est_bytes; /* estimativa dos volumes (após compressão) */
  uint64_t overhead_bytes;   /* manifesto + CRC + índice (folga incluída) */
  uint64_t sampled_bytes;    /* bytes lidos/comprimidos na amostra */
} ampr_estimate_t;
int ampr_pack_estimate(const scan_result_t *r, int level, int pack_all, ampr_estimate_t *est, char *err,
                       size_t errlen);

/* Decisão de codec de um bloco conforme _compress_block (layout mixed, io_page 64 KiB):
 * devolve 1 se o resultado LZ4 de tamanho comp deve ser mantido para um bloco de raw bytes. */
int ampr_keep_lz4(uint32_t raw, uint32_t comp, uint32_t io_page);
/* Igual, com limiares explícitos (min_savings_* / io_neutral_min_savings_*, razões em 1/10000). */
int ampr_keep_lz4_ex(uint32_t raw, uint32_t comp, uint32_t io_page, uint32_t min_bytes, uint32_t min_bp,
                     uint32_t io_min_bytes, uint32_t io_min_bp);

/* AMPRCFG1 com orçamento automático (ampr_pack_profile.c/ampr_pack.c da referência, runtime_budget):
 * 8 workers, 1 reservado para latência; caches calculados a partir do pool de 384 MiB do runtime
 * descontando manifesto e AMPRIDX3. */
#define AMPR_RT_WORKERS 8u
#define AMPR_RT_RESERVE_WORKERS 1u
#define AMPR_RT_POOL_BYTES (384ull << 20)
void ampr_runtime_encode(uint8_t out[AMPR_CFG1_SIZE], const uint8_t build_id[16], uint64_t manifest_len,
                         uint64_t idx_len);
