# Host build for the guiding test harnesses. For the board, cross-compile:
#   make CC=arm-rockchip830-linux-uclibcgnueabihf-gcc \
#        CFLAGS='-O2 -Wall -Wextra -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard'
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

CORE = guide_star.c guide_algo.c guide_source.c guide_cal.c guide_mount.c
HDRS = guide_star.h guide_algo.h guide_source.h guide_cal.h guide_mount.h

all: guide_sim guide_cal_test

CIVETWEB = ../../third_party/civetweb

guide_sim: guide_sim.c $(CORE) $(HDRS)
	$(CC) $(CFLAGS) -o $@ guide_sim.c $(CORE) -lm

guide_cal_test: guide_cal_test.c $(CORE) $(HDRS)
	$(CC) $(CFLAGS) -o $@ guide_cal_test.c $(CORE) -lm

# The /guide page and loop on a host, simulated source and mount. Not in "all":
# it pulls in civetweb, which the other harnesses do not need.
# Serves the embedded web/ by default; --web ../../web serves the files live
# from disk instead, for editing the page without rebuilding.
guide_serve: guide_serve.c guide_loop.c guide_api.c $(CORE) $(HDRS) guide_loop.h guide_api.h \
             ../web.c ../web_assets.c guide_settings.c guide_settings.h
	$(CC) $(CFLAGS) -DNO_SSL -DNO_CGI -I$(CIVETWEB) -I.. -o $@ guide_serve.c \
	    guide_loop.c guide_api.c guide_settings.c $(CORE) ../util.c ../web.c ../web_assets.c \
	    $(CIVETWEB)/civetweb.c -lpthread -lm

../web_assets.c: $(shell find ../../web -type f) ../../tools/embed_web.py
	python3 ../../tools/embed_web.py ../../web $@

# Calibration and closed-loop guiding against the simulated mount.
test: guide_cal_test
	./guide_cal_test

clean:
	rm -f guide_sim guide_cal_test guide_serve
.PHONY: all test clean
