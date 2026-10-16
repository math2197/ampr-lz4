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
make host            # build/ampr-lz4-host: com o núcleo real, no host
make ps5-stub        # só valida que a camada de plataforma compila para o PS5
make diag            # build/ampr-diag.elf (também sai no `make`): payload mínimo de diagnóstico
```

O ELF só depende das bibliotecas padrão do SDK (`libkernel_web`, `libSceLibcInternal`, `libSceNet`, igual ao
`hello_world`); `libSceAppInstUtil` só é usada pelo `ampr-shortcut.elf` separado. Confira com
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
```

O endereço da API do ShadowMountPlus vem de `/data/shadowmount/config.ini` (`api_port`, `api_bind_address`).

## Atalho em Mídia

O atalho é um payload **separado**, `build/ampr-shortcut.elf`: o `ampr-lz4.elf` não carrega `libSceAppInstUtil`
(neste ambiente carregá-la no processo do servidor o derrubava). Se o instalador falhar, o servidor não cai.

1. Com o `ampr-lz4.elf` já rodando (ou não, tanto faz), envie `build/ampr-shortcut.elf` uma vez
   (`nc <ps5> 9021 < build/ampr-shortcut.elf`). Ele grava `/user/app/AMLZ00001/sce_sys/{param.json,icon0.png}`,
   registra o app com `sceAppInstUtilAppInstallTitleDir` e sai.
2. O tile **AMPR-LZ4** aparece na aba **Mídia** e abre `http://127.0.0.1:<porta>/` (porta lida de
   `/data/ampr-lz4/config.ini`, padrão 8095). Se mudar a `port`, reenvie o `ampr-shortcut.elf`.
3. Notificações no início e no fim mostram o resultado (com o código de erro, se falhar).
4. Log: `/data/ampr-lz4/ampr-shortcut.log`, com uma linha **antes** de cada passo arriscado ("vou chamar
   sceAppInstUtilInitialize", etc.), gravada com `fsync`. A última linha mostra onde o processo morreu, se morrer.

- O atalho só abre a web UI; ela é servida pelo `ampr-lz4.elf`, que precisa estar em execução.
- Se o tile não aparecer, pode ser preciso reiniciar a tela inicial (ou o console).
- A chave `shortcut` do `config.ini` foi removida; se ainda existir no arquivo, é ignorada.
- Remover: **Configurações > Armazenamento** (apagar o app AMPR-LZ4) ou apagar o app pela tela inicial.
- O ícone (`assets/icon0.png`, 1024x1024) é gerado por `python3 -I tools/make_icon.py` e embutido no ELF.
- Build: `make shortcut` (linkado como o `ftpsrv-ps5-install.elf`: `-lSceIpmi -lSceAppInstUtil`).

## Solução de problemas

**Log**: `/data/ampr-lz4/ampr-lz4.log` (uma linha por etapa: entrada em `main`, config, socket/bind/listen com
errno, thread de conversão, todas as notificações e a escolha do `libSceAmpr.sprx`).
É gravado com `fsync` a cada linha e zerado quando passa de 1 MiB. As mesmas linhas vão para o stdout (alguns
loaders devolvem o stdout na conexão do `nc`). Baixe por FTP (ex.: ftpsrv, porta 2121).

Falhas de inicialização viram notificação com o errno (ex.: `falha ao abrir a porta 8095 (errno N: ...)`,
`já está em execução (porta 8095 em uso)`, (o `ampr-shortcut.elf` avisa por conta própria)).

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

## Licença

GPL-3.0. Inclui `assets/libSceAmpr.sprx` do [ampr_emu](https://github.com/drakmor/ampr_emu) 0.4.2.1
(GPL-3.0, sem modificação; origem e SHA-256 em `assets/libSceAmpr.ORIGIN.txt`).
