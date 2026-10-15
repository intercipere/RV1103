CROSS    ?= arm-rockchip830-linux-uclibcgnueabihf-
CC        = $(CROSS)gcc
CIVETWEB  = third_party/civetweb
CFLAGS    = -O2 -Wall -Wextra -DNO_SSL -DNO_CGI -I$(CIVETWEB) -Isrc -Isrc/guide
LDFLAGS   = -lpthread -lm
# src/guide/ holds the ported guiding core. guide_sim.c is the standalone test
# harness (it has its own main) and guide_source.c's HTTP camera path is for
# host-side testing, so neither belongs in the daemon.
GUIDE_SRC = src/guide/guide_star.c src/guide/guide_algo.c \
            src/guide/guide_source.c src/guide/guide_source_v4l2.c \
            src/guide/guide_cal.c src/guide/guide_mount.c src/guide/guide_settings.c \
            src/guide/guide_loop.c src/guide/guide_api.c
# web/ is the browser UI, embedded into the binary pre-gzipped. The .c is
# generated -- edit web/, never src/web_assets.c.
WEB_FILES = $(shell find web -type f)
WEB_C     = src/web_assets.c
SRC       = $(filter-out src/test_%.c $(WEB_C),$(wildcard src/*.c)) $(WEB_C) \
            $(GUIDE_SRC) $(CIVETWEB)/civetweb.c
BIN       = alpacad

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

$(WEB_C): $(WEB_FILES) tools/embed_web.py
	python3 tools/embed_web.py web $@

clean:
	rm -f $(BIN) $(WEB_C)

.PHONY: clean
