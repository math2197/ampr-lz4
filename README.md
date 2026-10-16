# AMPR-LZ4

Payload para PS5 (`ampr-lz4.elf`) que converte jogos em imagem `.exfat` / `.ffpfsc` para o formato
AMPR com compressão LZ4 e oferece uma interface web para escolher o jogo, o destino e acompanhar o progresso.

## Requisitos

- PS5 com payloads habilitados e **kstuff**.
- **ShadowMountPlus 1.7+** em execução, com a API ativa (`api_enabled=1`, padrão; porta 10101).
  Os jogos precisam já estar registrados/gerenciados pelo ShadowMountPlus.
- Espaço livre no destino (`/data`, `/mnt/ext0` ou `/mnt/ext1`).

## Compilar

```sh
make                 # ampr-lz4.elf (requer ps5-payload-sdk; PS5_PAYLOAD_SDK=~/ps5-payload-sdk)
make host-stub       # build/ampr-lz4-stub: servidor com conversão simulada (testa UI e API no PC)
make host            # build/ampr-lz4-host: servidor com o núcleo real, no host (usa o SMP/imagens)
make -C tests        # tests/build/host_convert: CLI do núcleo (converte uma pasta), e test_hashes
make ps5-stub        # só valida que a camada de plataforma compila para o PS5
make diag            # build/ampr-diag.elf (também sai no `make`): payload mínimo de diagnóstico
```

O ELF é linkado com `-lSceIpmi -lSceAppInstUtil` (nessa ordem, como o ftpsrv), usadas pelo atalho em Mídia, além
das padrão do SDK (`libkernel_web`, `libSceLibcInternal`, `libSceNet`). Confira com
`llvm-readelf -d ampr-lz4.elf | grep NEEDED`.

Teste no PC: `python3 -I tools/mock_smp.py` (mock do SMP) e
`AMPR_ROOT_PREFIX=/pasta/teste build/ampr-lz4-stub` (redireciona `/data` e `/mnt/extN`); abra `http://localhost:8095`.

## Enviar ao PS5

```sh
nc <ip-do-ps5> 9021 < ampr-lz4.elf      # ou: make test PS5_HOST=<ip>
```

Aparecem as notificações `AMPR-LZ4 0.1.0 iniciando...` (logo ao entrar em `main`) e
`AMPR-LZ4 pronto: http://<ip>:8095`.

## Usar

1. Abra o endereço no navegador (celular ou PC na mesma rede).
2. Escolha o jogo, o drive de destino (`data`, `ext0`, `ext1`) e o nível LZ4 (1-12; 1-4 rápido, 5-12 HC).
3. Acompanhe: montar, analisar, compactar, copiar, verificar. O resultado vai para
   `<drive>/homebrew/<TITLEID>-AMPR`. Ao final o jogo é desmontado (só se foi o AMPR-LZ4 que montou) e, se a
   fakelib estava desativada, ela é ajustada para `emulators`.
   O `fakelib/libSceAmpr.sprx` do destino é sempre um emulador que entende pacotes (string `AMPRPAK4`):
   o de `emulators_path` do SMP (padrão `/data/shadowmount/emus`) se existir e for compatível; senão o
   embutido no ELF (ampr_emu 0.4.2.1, gravado em `/data/ampr-lz4/libSceAmpr.sprx`). O `libSceAmpr.sprx`
   antigo que vier na `fakelib/` do jogo é substituído (versões antigas só têm `AMPRIDX3` e o jogo convertido
   crasha com 0xa0020318); os demais arquivos da `fakelib/` (ex.: `libSceAgc.sprx`) são copiados como estão.

Só aceita conexões de 127.0.0.1 e redes privadas (10/8, 172.16/12, 192.168/16, 169.254/16).

## Configuração (opcional)

`/data/ampr-lz4/config.ini`:

```ini
port=8095      # porta HTTP
workers=4      # threads de compressão (1-16)
level=6        # nível LZ4 padrão (1-12)
shortcut=1     # 0 desliga a criação do atalho em Mídia
keep_loose=    # avançado: extensões mantidas fora dos pacotes, separadas por vírgula (ex.: .ucas,.rmdtoc)
pack_all=0     # 1 desliga o auto-loose em todas as conversões (ver "Compatibilidade e perfis")
```

`pack_all` também pode ser pedido por conversão: checkbox **Empacotar mesmo sem ganho** em **Avançado** na web
UI, ou `"pack_all": true` no JSON de `POST /api/convert`. Vale o OU entre o `config.ini` e o pedido.

O endereço da API do ShadowMountPlus vem de `/data/shadowmount/config.ini` (`api_port`, `api_bind_address`).

## Atalho em Mídia

O próprio `ampr-lz4.elf` cria o atalho no primeiro envio, sem payload extra: numa thread separada ele grava
`/user/app/AMLZ00001/sce_sys/{param.json,icon0.png}` e registra o app com `sceAppInstUtilAppInstallTitleDir`.

- O tile **AMPR-LZ4** aparece na aba **Mídia** e abre `http://127.0.0.1:<porta>/` (porta do `config.ini`, padrão 8095).
- Nos envios seguintes ele só reinstala se o `param.json` (por exemplo, após mudar a `port`) ou o `icon0.png` mudarem;
  senão o log registra "já instalado".
- Uma notificação avisa "atalho em Mídia criado" (primeira vez) ou "atalho falhou (<etapa> 0x...)".
- Log: `/data/ampr-lz4/ampr-lz4.log`, com uma linha `atalho: vou ...` **antes** de cada passo arriscado
  (gravada com `fsync`); a última linha mostra onde o processo morreu, se morrer.
- Desligar: `shortcut=0` no `config.ini`.
- Remover: apague o app AMPR-LZ4 em **Configurações > Armazenamento** (ou pela tela inicial) e coloque `shortcut=0`,
  senão ele é recriado no próximo envio.
- O atalho só abre a web UI; ela é servida pelo `ampr-lz4.elf`, que precisa estar em execução.
- Se o tile não aparecer, pode ser preciso reiniciar a tela inicial (ou o console).
- O ícone (`assets/icon0.png`, 1024x1024) é gerado por `python3 -I tools/make_icon.py` e embutido no ELF.

## Solução de problemas

**Log**: `/data/ampr-lz4/ampr-lz4.log` (uma linha por etapa: entrada em `main`, config, socket/bind/listen com
errno, thread de conversão, todas as notificações e a escolha do `libSceAmpr.sprx`).
É gravado com `fsync` a cada linha e zerado quando passa de 1 MiB. As mesmas linhas vão para o stdout (alguns
loaders devolvem o stdout na conexão do `nc`). Baixe por FTP (ex.: ftpsrv, porta 2121).

Falhas de inicialização viram notificação com o errno (ex.: `falha ao abrir a porta 8095 (errno N: ...)`,
`já está em execução (porta 8095 em uso)`).

**Nenhuma notificação ao enviar o `ampr-lz4.elf`**: o payload não chegou a `main`. Teste o ambiente com o
payload de diagnóstico, que é linkado exatamente como o `hello_world` do SDK:

```sh
nc <ip-do-ps5> 9021 < build/ampr-diag.elf
```

Ele mostra `diag 1`, grava `/data/ampr-lz4/diag.txt` (`diag 2`), abre `0.0.0.0:8095` e notifica o resultado
(`diag 3`), responde `ok` por 60 s (teste: `curl http://<ip-do-ps5>:8095/` no PC) e sai (`diag 4`).

- `diag` não notifica nada: o loader/HEN não está executando payloads do SDK (problema de ambiente).
- `diag` funciona e o `ampr-lz4.elf` não notifica nem cria o log: anote o loader usado e a versão do firmware
  e mande junto com o que aparecer no `nc` (stdout).
- `diag 3` com erro de bind: outro processo usa a porta 8095 (mude `port=` em `config.ini`).

## Compatibilidade e perfis

O que vai para os pacotes AMPR e o que fica solto (lido direto do disco) é decidido por camadas; a primeira
que se aplica vence:

1. **Segurança e sempre-em-disco** (nada muda isso):
   - extensões: executáveis/módulos (`.elf .self .prx .sprx`), `eboot.bin` em qualquer pasta,
     configuração e metadados (`.bin .dat .json .xml .ini .cfg .txt .bak .utoc`), mídia (vídeo/áudio:
     `.mp4 .bk2 .usm .webm .ivf .mp3 .ogg .wav .flac .aac .opus .m4a .at9 .avi .mkv .mov .wmv .flv .m4v`),
     bancos de som (`.bank .fsb .wem .bnk .resource`), imagens/texturas soltas
     (`.png .jpg .jpeg .webp .gif .bmp .ico .tga .tif .tiff .exr .hdr .psd .dds .ktx .ktx2 .astc .basis .gnf .gnfp .jxm .vtf`)
     e arquivos já comprimidos ou imagens (`.zip .7z .rar .pak .gz .xz .bz2 .zst .lz4 .pfs .img`);
   - pastas `sce_sys`, `sce_module`, `fakelib`, `system`, `save`, `mods`, `trophy2`, `uds`, `decrypted` em
     **qualquer** profundidade, e `_DUPLEX_/` na raiz;
   - arquivos vazios, marcadores do PlayGo (`cache_ps5/pgc_*_dummy_file`, `cache_ps5/playgo.pgm`), os arquivos
     de boot do Ghost of Yōtei (`cache_ps5/*.texmeshman|packman`, `sounds/scream/_generated/*`) e o
     `packagedefinition.txt` do Glacier (HITMAN 3, 007 First Light).
2. **`keep_loose`** do usuário (`config.ini` ou pedido).
3. **Perfil do jogo** embutido (`profiles/`): escolhido pelo Title ID de `sce_sys/param.json`, depois pelo nome
   do título, depois pelo layout dos arquivos. Ele define `default_action`, as regras `compress`/`loose` (glob
   com `*`, `**` e `?`; `*` também casa `/`; a última regra que casa vence), `block_size` e as flags
   (`hot`, `layout = "random"`). Com um perfil, tudo o que ele não lista segue o `default_action` (solto nos
   quatro perfis atuais).

   | Perfil | Escolhido por | Testado em |
   |---|---|---|
   | `insomniac` | PPSA03671, nome "wolverine", ou `toc` + `dag` + `d/` na raiz | Marvel's Wolverine 01.001.005 |
   | `unity` | PPSA28997, nome "sonsofsparta", ou `StreamingAssets/` ao lado de `Data/globalgamemanagers` (raiz ou `Media/`) | God of War Sons of Sparta 01.008.001 |
   | `astrobot` | PPSA21567, nome "astrobot" | ASTRO BOT 01.019.000 |
   | `ufc6` | PPSA23566, nome "ufc6" | EA SPORTS UFC 6 01.004.000 |

   No Unity só `StreamingAssets/*` é comprimido; `level*`, `sharedassets*`, `globalgamemanagers`, `.resS` e
   `global-metadata.dat` (o IL2CPP faz mmap dele) ficam soltos. No Insomniac só `d/*` é comprimido (blocos de
   128 KiB), com `bootload*`, `critbootload*`, `movie*`, `soundbank*`, `wem*`, `toc` e `dag` soltos.
4. **Perfil da engine** (sem perfil de jogo):
   - **Unreal**: `.ucas` (dados do IoStore), `.uasset` e `.ubulk` vão para os pacotes, como no
     PS5-LZ4-Compression. Ficam soltos `.utoc`, `.pak` (já soltos pela camada 1), `.sig`, o container global
     (`global.ucas`/`global.utoc`, em qualquer pasta, sem distinção de maiúsculas), `.uproject`,
     `.upluginmanifest`, `.uplugin`, `.locres`, `.locmeta`, `.icu`, `*/Internationalization/` (dados do ICU) e
     tudo sob `*/Config/` (e `.ini`/`.json`/`.cfg`/`.dat` pela camada 1). Antes, `Paks/` inteiro ficava solto
     e quase nada de um jogo como Stellar Blade ia para os pacotes. O SIGSEGV visto no Stellar Blade
     ("Failed to find /Engine/...") foi com um build que empacotava também `Config/`, `.ini` e `.uproject`;
     o container global fica solto por precaução. Para voltar ao comportamento antigo: `keep_loose=.ucas`.
     Os `.ucas` costumam já vir comprimidos (Oodle), então a redução de tamanho pode ser pequena e vários
     podem sair pelo auto-loose (camada 5); use `pack_all` para empacotá-los mesmo assim.
   - **Northlight**: `.rmdtoc` solto.
5. **Regras padrão**: arquivos da raiz soltos; contêineres de streaming (`movie*`, `soundbank`, `soundbank_*`,
   `wem*`) soltos; pastas de áudio (`audio`, `sound(s)`, `sfx`, `bgm`, `music`, `voice(s)`, `snd`, `vo`,
   `dialog(ue)`) e `soundbank.*` comprimidos em blocos de 16 KiB com `HOT|RANDOM_ACCESS` (nunca `STREAMING`);
   o resto comprimido em blocos de 64 KiB.
6. **Auto-loose** (etapa de compactação): arquivos de 64 MiB ou mais (exceto `HOT`) são amostrados (até 32
   blocos / 16 MiB) e ficam soltos se a economia for menor que 5% ou se 90% ou mais dos blocos não
   comprimirem. Com `pack_all` (`config.ini`, checkbox da web UI, `"pack_all": true` na API ou
   `--pack-all` na CLI) essa etapa é pulada: tudo o que as camadas 1-5 mandaram para os pacotes vai para os
   pacotes, mesmo sem ganho de tamanho (blocos sem ganho são gravados crus). Não muda as camadas 1-5.

A web UI mostra durante a conversão "X GB em pacotes LZ4 / Y GB soltos", já descontando o auto-loose. No
`/api/status`, `current.packed_bytes`/`loose_bytes` são a estimativa da análise e `current.auto_loose_bytes`
o que o auto-loose tirou dos pacotes depois (em pacotes = `packed_bytes - auto_loose_bytes`; soltos =
`loose_bytes + auto_loose_bytes`); `current.pack_all` indica se o auto-loose está desligado.

### CLI no host (pasta já extraída)

`tests/build/host_convert` (`make -C tests`) converte uma **pasta** de jogo (com `eboot.bin` e `sce_sys/`),
não imagens `.exfat`/`.ffpfsc` (monte a imagem antes). O destino não pode existir.

```sh
tests/build/host_convert [-l|--level 1-12] [-w|--workers 1-16] [-f|--fakelib libSceAmpr.sprx] \
                         [-k|--keep-loose lista] [-a|--pack-all] <origem> <destino>
# ex.: tests/build/host_convert -l 6 -w 8 -f assets/libSceAmpr.sprx --pack-all -k .bk2 /Volumes/PPSA00000 ~/out/PPSA00000-AMPR
```

O perfil escolhido aparece no log (`AMPR: perfil insomniac (por layout; ...)`), no progresso da web UI ao lado
da engine e no campo `profile` do `/api/status` (`current` e `history`; `null` sem perfil).

Outros detalhes da saída:

- **Permissões**: tudo o que é gravado no destino (pacotes, manifestos, arquivos soltos, `fakelib/`, pastas)
  fica com modo `0777`. Cópias de imagens `.exfat` chegam como `0666`/`0644` e um `eboot.bin` ou módulo sem
  bit de execução derruba o jogo com SIGBUS / CE-108255-1.
- **`ampr_assets.index.runtime` (AMPRCFG1)**: 8 workers, 1 reservado para latência e caches calculados
  automaticamente a partir do pool de 384 MiB do runtime (descontando manifesto e `ampr_emu.index`), como no
  PS5-LZ4-Compression.
- **PlayGo**: se o `eboot.bin` (ou um `.prx`/`.sprx` de `sce_module/`) importa `libScePlayGo` e não há
  `fakelib/libScePlayGo.sprx` na origem nem `libScePlayGo.sprx` na pasta de emuladores do SMP, a conversão
  termina com um aviso: rodando de pasta, `scePlayGoOpen` falha e jogos que conferem isso param com SIGBUS no
  boot. O AMPR-LZ4 **não** inclui um stub de PlayGo (a referência também não distribui um; ela usa um emulador
  já instalado no console): instale um em `fakelib/` do jogo ou na pasta de emuladores do SMP.
- Jogos sem `libSceAmpr` (ex.: o backport de Hollow Knight) continuam recusados na checagem de compatibilidade.

Para acrescentar um perfil: coloque o `.toml` em `profiles/`, uma entrada em `profiles/index.json` e recompile
(o Makefile embute tudo em `src/profiles_asset.h`). O parser aceita só o subconjunto de TOML usado por esses
arquivos; chaves de `[runtime]`, `[groups.*]` e de nomes/níveis de `[pack]` são ignoradas (o empacotador usa
grupo único, layout `mixed` e o orçamento automático acima; `layout = "streaming"` vira `mixed` e
`action = "store"` vira solto).

## Licença

GPL-3.0. Inclui `assets/libSceAmpr.sprx` do [ampr_emu](https://github.com/drakmor/ampr_emu) 0.4.2.1
(GPL-3.0, sem modificação; origem e SHA-256 em `assets/libSceAmpr.ORIGIN.txt`).

### Créditos

- **drakmor** — [ampr_emu](https://github.com/drakmor/ampr_emu): o emulador embutido, os formatos AMPR e as
  ferramentas de referência em `tests/ref/`.
- **Husam Osman (theghostonline)** — [PS5-LZ4-Compression](https://github.com/theghostonline/PS5-LZ4-Compression)
  (GPL-3.0-or-later): lista de segurança, arquivos sempre em disco, permissões 0777, orçamento do AMPRCFG1,
  regras de áudio, seleção de perfis e checagem de PlayGo foram adaptados de lá.
- **danthespal** — [MkPFS.CSharp](https://github.com/danthespal/MkPFS.CSharp) (GPL-3.0): os perfis TOML em
  `profiles/`.

Detalhes do que veio de onde em [`THIRD_PARTY.md`](THIRD_PARTY.md).
