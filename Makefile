CFLAGS ?= -O2 -Wall -Wno-deprecated-declarations
LIBS = -lcups
# CUPS 2.3 on Linux keeps the raster API in libcupsimage. macOS has it in libcups.
ifeq ($(shell uname -s),Linux)
LIBS += -lcupsimage
endif
ifeq ($(shell uname -s),Darwin)
LZO_PREFIX ?= $(shell brew --prefix 2>/dev/null)
ifneq ($(wildcard $(LZO_PREFIX)/lib/liblzo2.a),)
LZO_CFLAGS ?= -I$(LZO_PREFIX)/include
# Static, so the filter does not load from /opt/homebrew inside the cupsd sandbox.
LZO_LIBS ?= $(LZO_PREFIX)/lib/liblzo2.a
endif
endif

# LZO enables BITMAP compression (DataCompress). Build with LZO=0 to leave it out.
LZO ?= $(shell printf '\043include <lzo/lzo1x.h>\n' | $(CC) $(LZO_CFLAGS) -E - >/dev/null 2>&1 && echo 1 || echo 0)
# Kept out of CFLAGS so "make CFLAGS=..." does not drop -DHAVE_LZO.
ifeq ($(LZO),1)
DEFS = -DHAVE_LZO $(LZO_CFLAGS)
LIBS += $(if $(LZO_LIBS),$(LZO_LIBS),-llzo2)
endif

all: raster-tspl ppds

raster-tspl: raster-tspl.c
	$(CC) $(CFLAGS) $(DEFS) -o $@ $< $(LIBS)

ppds: ppd/comer-tspl.drv
	ppdc -d build ppd/comer-tspl.drv

clean:
	rm -rf raster-tspl build

.PHONY: all ppds clean
