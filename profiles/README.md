# Perfis embutidos

Perfis de empacotamento no formato TOML do `ampr_pack.py` ([ampr_emu](https://github.com/drakmor/ampr_emu),
drakmor). O AMPR-LZ4 escolhe um automaticamente pelo Title ID (`sce_sys/param.json`), pelo nome do título ou
pelo layout dos arquivos, conforme `index.json`, e o Makefile embute todos no ELF (`src/profiles_asset.h`).

| Perfil | Escolhido para | Testado em | Origem |
|---|---|---|---|
| `astrobot.toml` | ASTRO BOT | PPSA21567, 01.019.000 | [MkPFS.CSharp #5](https://github.com/danthespal/MkPFS.CSharp/issues/5) |
| `ufc6-PPSA23566.toml` | EA Sports UFC 6 | PPSA23566, 01.004.000 | [MkPFS.CSharp #2](https://github.com/danthespal/MkPFS.CSharp/issues/2) |
| `insomniac.toml` | jogos Insomniac (`toc`, `dag` e `d/` na raiz) | Marvel's Wolverine (PPSA03671, 01.001.005) | [MkPFS.CSharp #4](https://github.com/danthespal/MkPFS.CSharp/issues/4) |
| `unity.toml` | jogos Unity (`StreamingAssets` ao lado dos `Data/` do Unity) | God of War Sons of Sparta (PPSA28997, 01.008.001) | [MkPFS.CSharp #3](https://github.com/danthespal/MkPFS.CSharp/issues/3) |

Créditos: perfis escritos e testados por **danthespal** e publicados com
[MkPFS.CSharp](https://github.com/danthespal/MkPFS.CSharp) (GPL-3.0); distribuídos pelo
[PS5-LZ4-Compression](https://github.com/theghostonline/PS5-LZ4-Compression) de **Husam Osman**
(GPL-3.0-or-later), de onde vieram estes arquivos (commit `9be57b7`) e o `index.json`; formato e ferramentas do
**ampr_emu** de **drakmor** (GPL-3.0). Os `.toml` estão sem alteração, exceto pelo cabeçalho de origem.

Todo perfil deixa solto o que não nomeia, e a lista de segurança do AMPR-LZ4 (README, "Compatibilidade e
perfis") vale por cima de qualquer perfil. Para acrescentar um: coloque o `.toml` aqui, uma entrada em
`index.json` e recompile.
