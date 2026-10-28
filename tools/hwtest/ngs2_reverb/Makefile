# OpenOrbis PS4 toolchain build (https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain), Linux host.
# OO_PS4_TOOLCHAIN must point at the toolchain; the result is IV0000-PSPT01097_00-NGS2REVERB000000.pkg.
TOOLCHAIN := $(OO_PS4_TOOLCHAIN)
TITLE := Ngs2 reverb test
VERSION := 1.00
TITLE_ID := PSPT01097
CONTENT_ID := IV0000-PSPT01097_00-NGS2REVERB000000
BIN := $(TOOLCHAIN)/bin/linux

CFLAGS := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O1 -Wall -Wextra -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include
LDFLAGS := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr -L$(TOOLCHAIN)/lib
LIBS := -lc -lkernel -lSceSysmodule -lSceNgs2

all: $(CONTENT_ID).pkg

build/main.o: main.c
	mkdir -p build
	clang $(CFLAGS) -c $< -o $@

eboot.bin: build/main.o
	ld.lld $(LDFLAGS) $(TOOLCHAIN)/lib/crt1.o $< $(LIBS) -o build/ngs2_reverb.elf
	$(BIN)/create-fself -in=build/ngs2_reverb.elf -out=build/ngs2_reverb.oelf --eboot eboot.bin --paid 0x3800000000000011

sce_sys/param.sfo: Makefile
	mkdir -p sce_sys
	$(BIN)/PkgTool.Core sfo_new $@
	$(BIN)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(BIN)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(BIN)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(BIN)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(BIN)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(BIN)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(BIN)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(BIN)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(BIN)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(BIN)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

sce_sys/about/right.sprx:
	mkdir -p sce_sys/about
	cp $(TOOLCHAIN)/samples/hello_world/sce_sys/about/right.sprx $@

sce_sys/icon0.png:
	cp $(TOOLCHAIN)/samples/hello_world/sce_sys/icon0.png $@

pkg.gp4: eboot.bin sce_sys/param.sfo sce_sys/about/right.sprx sce_sys/icon0.png
	$(BIN)/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$^"

$(CONTENT_ID).pkg: pkg.gp4
	$(BIN)/PkgTool.Core pkg_build $< .

clean:
	rm -rf build sce_sys eboot.bin pkg.gp4 $(CONTENT_ID).pkg

.PHONY: all clean
