# Código e dados de terceiros

O AMPR-LZ4 é GPL-3.0. Todo o material abaixo tem licença compatível (GPL-3.0 ou GPL-3.0-or-later) e mantém
os créditos originais nos arquivos.

## ampr_emu (drakmor) — GPL-3.0

<https://github.com/drakmor/ampr_emu>

| Nosso arquivo | Origem |
|---|---|
| `assets/libSceAmpr.sprx` | release 0.4.2.1, sem modificação (SHA-256 em `assets/libSceAmpr.ORIGIN.txt`) |
| `tests/ref/*.py` | `tools/ampr_pack.py`, `tools/ampr_pack_format.py`, `tools/build_ampr_index.py` (usados só nos testes) |
| `src/ampr_pack.c`, `src/ampr_index.c`, `src/verify.c` | reimplementação em C dos formatos AMPRPAK4/AMPRDAT3/AMPRCRC1/AMPRCFG1/AMPRIDX3 seguindo as ferramentas acima |
| `profiles/*.toml` | o formato dos perfis é o do `ampr_pack.py`; `astrobot.toml` foi gerado pelo `ampr_pack_profile.py` |

## PS5-LZ4-Compression (Husam Osman / theghostonline) — GPL-3.0-or-later

<https://github.com/theghostonline/PS5-LZ4-Compression>, commit `9be57b7`.
`SPDX-License-Identifier: GPL-3.0-or-later`, `Copyright (C) 2026 Husam Osman`.

| Nosso arquivo | Origem | O que foi adaptado |
|---|---|---|
| `src/scan.c` (`k_loose_ext`, `k_loose_dirs`, `k_loose_names`) | `src/ampr_pack_profile.c:1180-1268` (`k_safety_exclusions`) | lista de segurança: extensões e pastas sempre soltas |
| `src/scan.c` (`k_keep_on_disk`) | `src/ampr_pack.c:1403-1427` (`k_keep_on_disk`, `ampr_pack_keep_on_disk`) | PlayGo e arquivos de boot do Ghost of Yōtei sempre no disco |
| `src/scan.c` (`k_audio_dirs`, `k_stream_containers`, `default_classify`) | `src/ampr_pack_profile.c:1430-1641` (`ap_config_build_universal`) | áudio em blocos de 16 KiB com `hot` + layout random e limiares 16 B / 0,25 % / 2 KiB / 12,5 %; contêineres de streaming soltos |
| `src/ampr_pack.c` (`ampr_runtime_encode`) | `src/ampr_pack.c:80`, `:2178-2197` (`RUNTIME_POOL_BYTES`, `runtime_budget`) e `rt_workers = 8`, `rt_latency_reserve_workers = 1` | orçamento automático do AMPRCFG1 |
| `src/ampr_pack.c` (flags de arquivo) | `src/ampr_pack.c:1673-1678` (`item_file_flags`) | `HOT` e `RANDOM_ACCESS` |
| `src/convert.c` (`DST_MODE`), `src/util.c`, `src/ampr_pack.c` | `src/ampr_pack.c:2963-2972` e changelog 1.4.3 | permissões 0777 no destino |
| `src/scan.c` (`scan_layout_engine`), `src/profile.c` (seleção) | `src/lzc_api.c` (`detect_game_engine`, `bundled_profile_match`, `csv_has`, `csv_found_in`) | escolha do perfil por Title ID, nome e layout |
| `src/convert.c` (`playgo_check`) | `src/lzc_boot_files.c` (`title_imports`, `check_playgo`) | detecção de `libScePlayGo` sem `fakelib/libScePlayGo.sprx` (só aviso; a referência não distribui o stub) |
| `profiles/index.json` | `profiles/index.json` | copiado sem alteração |

## MkPFS.CSharp (danthespal) — GPL-3.0

<https://github.com/danthespal/MkPFS.CSharp>

Os perfis `profiles/astrobot.toml`, `profiles/insomniac.toml`, `profiles/unity.toml` e
`profiles/ufc6-PPSA23566.toml` foram escritos e testados por danthespal (issues #2 a #5 do MkPFS.CSharp) e
chegaram aqui pelo PS5-LZ4-Compression. Foram copiados sem alteração, exceto por um cabeçalho de comentário
com a origem e a licença.

## LZ4 (Yann Collet) — BSD-2-Clause

`third_party/lz4/`: biblioteca LZ4/LZ4HC, licença no próprio diretório.
