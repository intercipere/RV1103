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
            src/guide/guide_loop.c src/guide/guide_api.c
SRC       = $(filter-out src/test_%.c,$(wildcard src/*.c)) $(GUIDE_SRC) $(CIVETWEB)/civetweb.c
BIN       = alpacad

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(BIN)

.PHONY: clean
