CFLAGS ?= -O2 -Wall -Wno-deprecated-declarations
LIBS = -lcups
# CUPS 2.3 on Linux keeps the raster API in libcupsimage. macOS has it in libcups.
ifeq ($(shell uname -s),Linux)
LIBS += -lcupsimage
endif

raster-tspl: raster-tspl.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f raster-tspl
