/* Verificação pós-gravação de um conjunto AMPRPAK4 (relê volumes, decodifica, confere CRC1). GPL-3.0. */
#pragma once
#include <stddef.h>
#include "convert.h"

/* Lê o manifesto em manifest_path e o sidecar crc_path, valida cabeçalhos/CRCs/layout,
 * abre os volumes (nomes relativos a dst_dir), confere cabeçalhos AMPRDAT3 e relê cada chunk,
 * decodificando LZ4 e comparando o CRC32 do bloco decodificado.
 * p (opcional): bytes_total/bytes_done em bytes lógicos; cancelamento. 0 ok, -1 erro, -2 cancelado. */
int ampr_verify(const char *dst_dir, const char *manifest_path, const char *crc_path, conv_progress_t *p,
                char *err, size_t errlen);
