/* Gerador do AMPRIDX3 (ampr_emu.index). GPL-3.0. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "scan.h"

#define AMPR_IDX3_NAME "ampr_emu.index"
#define AMPR_IDX3_HEADER_SIZE 48
#define AMPR_IDX3_ENTRY_SIZE 24
#define AMPR_IDX3_SLOT_SIZE 16

/* Monta o AMPRIDX3 com os arquivos indexed de r (já ordenados por chave dobrada;
 * fileId = posição entre os indexados + 1). *out alocado com malloc. 0 ok. */
int ampr_idx3_build(const scan_result_t *r, uint8_t **out, size_t *out_len, char *err, size_t errlen);
