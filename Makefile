# AMPR-LZ4 - payload PS5 + web UI. Alvos: all (ELF PS5 + build/ampr-diag.elf + build/ampr-shortcut.elf), diag, host, host-stub, ps5-stub, test, clean.
PS5_PAYLOAD_SDK ?= $(HOME)/ps5-payload-sdk
PS5_HOST ?= ps5
PS5_PORT ?= 9021
# o toolchain precisa de clang/lld do homebrew no PATH (macOS)
export PATH := /opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$(PATH)

# prospero.mk define CC/LD/etc. do toolchain PS5; os alvos de host usam HOSTCC
HOSTCC ?= cc
ifneq ($(wildcard $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk),)
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

PLAT_SRC := src/main.c src/httpd.c src/smp.c src/jobs.c src/json.c src/platform.c src/applog.c
CORE_SRC := $(filter-out src/convert_stub.c $(PLAT_SRC),$(wildcard src/*.c)) \
            third_party/lz4/lz4.c third_party/lz4/lz4hc.c
STUB_SRC := $(PLAT_SRC) src/convert_stub.c

WARN   := -Wall -Wextra
CFLAGS_COMMON := -O2 -g $(WARN) -pthread -Isrc -Ithird_party/lz4
ELF    := ampr-lz4.elf
# Sem bibliotecas extras: só os DT_NEEDED padrão do SDK (libkernel_web, libSceLibcInternal, libSceNet).
# Este ELF NUNCA toca libSceAppInstUtil: o atalho em Mídia é o payload separado build/ampr-shortcut.elf.
PS5_LIBS :=
DIAG_ELF := build/ampr-diag.elf
SHORTCUT_ELF := build/ampr-shortcut.elf
GEN_H := src/ui_assets.h src/icon_asset.h src/ampr_sprx_asset.h

UI_NAMES := index.html app.css app.js
# usa ui/ quando existir; senão placeholders SOMENTE dentro de build/
UI_FILES := $(foreach f,$(UI_NAMES),$(if $(wildcard ui/$(f)),ui/$(f),build/placeholder/ui/$(f)))

.PHONY: all shortcut diag host host-stub ps5-stub test clean
all: $(ELF) $(DIAG_ELF) $(SHORTCUT_ELF)

build/placeholder/ui/%:
	@mkdir -p $(dir $@)
	@printf '<!doctype html><title>AMPR-LZ4</title><script src="/app.js"></script><link rel="stylesheet" href="/app.css"><h1>AMPR-LZ4 (placeholder)</h1>' > $@ && \
	  case "$@" in *app.css) echo 'body{font-family:sans-serif}' > $@;; *app.js) echo 'console.log("placeholder")' > $@;; esac

# src/ui_assets.h: nomes estaveis ui_index_html, ui_app_css, ui_app_js (+ _len)
src/ui_assets.h: $(UI_FILES) Makefile
	@echo "GEN $@"
	@{ echo '/* gerado pelo Makefile - nao editar */'; \
	   for f in $(UI_FILES); do \
	     n=ui_$$(basename $$f | tr '.' '_'); \
	     echo "static const unsigned char $$n[] = {"; xxd -i < $$f; echo "};"; \
	     echo "static const unsigned int $${n}_len = $$(wc -c < $$f | tr -d ' ');"; \
	   done; } > $@

# src/icon_asset.h: icon0_png[] / icon0_png_len (icone do atalho; regenere com tools/make_icon.py)
src/icon_asset.h: assets/icon0.png Makefile
	@echo "GEN $@"
	@{ echo '/* gerado pelo Makefile - nao editar */'; \
	   echo "static const unsigned char icon0_png[] = {"; xxd -i < assets/icon0.png; echo "};"; \
	   echo "static const unsigned int icon0_png_len = $$(wc -c < assets/icon0.png | tr -d ' ');"; } > $@

# src/ampr_sprx_asset.h: libSceAmpr_sprx[] / libSceAmpr_sprx_len (ampr_emu 0.4.2.1; ver assets/libSceAmpr.ORIGIN.txt)
src/ampr_sprx_asset.h: assets/libSceAmpr.sprx Makefile
	@echo "GEN $@"
	@{ echo '/* gerado pelo Makefile - nao editar */'; \
	   echo "static const unsigned char libSceAmpr_sprx[] = {"; xxd -i < assets/libSceAmpr.sprx; echo "};"; \
	   echo "static const unsigned int libSceAmpr_sprx_len = $$(wc -c < assets/libSceAmpr.sprx | tr -d ' ');"; } > $@

# payload de diagnóstico: compilado/linkado como o samples/hello_world do SDK
diag: $(DIAG_ELF)
$(DIAG_ELF): tools/diag/diag.c
	@mkdir -p build
	$(CC) -Wall -Werror -g -o $@ $^

# payload separado do atalho em Mídia, linkado como o ftpsrv-ps5-install.elf (-lSceIpmi -lSceAppInstUtil)
shortcut: $(SHORTCUT_ELF)
$(SHORTCUT_ELF): tools/shortcut/shortcut.c src/icon_asset.h
	@mkdir -p build
	$(CC) -Wall -Wextra -Werror -g -Isrc -o $@ tools/shortcut/shortcut.c -lSceIpmi -lSceAppInstUtil

$(ELF): $(PLAT_SRC) $(CORE_SRC) $(GEN_H)
	$(CC) $(CFLAGS_COMMON) -DAMPR_PS5 -o $@ $(PLAT_SRC) $(CORE_SRC) $(PS5_LIBS)

ps5-stub: build/ampr-lz4-ps5-stub.elf
build/ampr-lz4-ps5-stub.elf: $(STUB_SRC) $(GEN_H)
	@mkdir -p build
	$(CC) $(CFLAGS_COMMON) -DAMPR_PS5 -o $@ $(STUB_SRC) $(PS5_LIBS)

host: build/ampr-lz4-host
build/ampr-lz4-host: $(PLAT_SRC) $(CORE_SRC) $(GEN_H)
	@mkdir -p build
	$(HOSTCC) $(CFLAGS_COMMON) -o $@ $(PLAT_SRC) $(CORE_SRC)

host-stub: build/ampr-lz4-stub
build/ampr-lz4-stub: $(STUB_SRC) $(GEN_H)
	@mkdir -p build
	$(HOSTCC) $(CFLAGS_COMMON) -o $@ $(STUB_SRC)

test: $(ELF)
	nc $(PS5_HOST) $(PS5_PORT) < $(ELF)

clean:
	rm -rf build $(ELF) $(GEN_H)
