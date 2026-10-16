# Build host (macOS/Linux) do núcleo AMPR, da CLI e dos testes. Uso: make -C tests [test]
CC ?= cc
ROOT := ..
CFLAGS ?= -O2 -g -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
CPPFLAGS += -I$(ROOT)/src -I$(ROOT)/third_party/lz4
LDLIBS += -lpthread
BUILD := build

CORE := util json profile scan ampr_index ampr_pack verify convert
CORE_OBJ := $(addprefix $(BUILD)/,$(addsuffix .o,$(CORE))) $(BUILD)/lz4.o $(BUILD)/lz4hc.o

all: $(BUILD)/host_convert $(BUILD)/test_hashes

$(BUILD):
	mkdir -p $@

# cabeçalho gerado pelo Makefile da raiz (perfis embutidos)
$(ROOT)/src/profiles_asset.h: $(wildcard $(ROOT)/profiles/*)
	$(MAKE) -C $(ROOT) src/profiles_asset.h

$(BUILD)/%.o: $(ROOT)/src/%.c $(wildcard $(ROOT)/src/*.h) $(ROOT)/src/profiles_asset.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/lz4.o: $(ROOT)/third_party/lz4/lz4.c | $(BUILD)
	$(CC) $(CPPFLAGS) -O3 -c -o $@ $<

$(BUILD)/lz4hc.o: $(ROOT)/third_party/lz4/lz4hc.c | $(BUILD)
	$(CC) $(CPPFLAGS) -O3 -c -o $@ $<

$(BUILD)/host_convert: $(ROOT)/tools/host_convert.c $(CORE_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_hashes: test_hashes.c $(CORE_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

test: all
	./$(BUILD)/test_hashes
	python3 -I run_e2e.py
	$(MAKE) -C $(ROOT) host
	python3 -I run_delete_source.py

# Só compila os objetos com o toolchain do PS5 (sem linkar).
PS5_SDK ?= $(HOME)/ps5-payload-sdk
ps5-objs:
	mkdir -p $(BUILD)/ps5
	for f in $(CORE); do PATH=/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$$PATH \
	  $(PS5_SDK)/bin/prospero-clang $(CPPFLAGS) -Wall -O2 -c -o $(BUILD)/ps5/$$f.o $(ROOT)/src/$$f.c || exit 1; done
	for f in lz4 lz4hc; do PATH=/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$$PATH \
	  $(PS5_SDK)/bin/prospero-clang $(CPPFLAGS) -Wall -O2 -c -o $(BUILD)/ps5/$$f.o $(ROOT)/third_party/lz4/$$f.c || exit 1; done

clean:
	rm -rf $(BUILD)

.PHONY: all test clean ps5-objs
