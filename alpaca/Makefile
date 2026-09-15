CROSS    ?= arm-rockchip830-linux-uclibcgnueabihf-
CC        = $(CROSS)gcc
CIVETWEB  = third_party/civetweb
CFLAGS    = -O2 -Wall -Wextra -DNO_SSL -DNO_CGI -I$(CIVETWEB) -Isrc
LDFLAGS   = -lpthread -lm
SRC       = $(filter-out src/test_%.c,$(wildcard src/*.c)) $(CIVETWEB)/civetweb.c
BIN       = alpacad

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(BIN)

.PHONY: clean
