# Host build for the guiding test harness. For the board, cross-compile:
#   make CC=arm-rockchip830-linux-uclibcgnueabihf-gcc \
#        CFLAGS='-O2 -Wall -Wextra -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard'
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

guide_sim: guide_sim.c guide_star.c guide_algo.c guide_source.c guide_star.h guide_algo.h guide_source.h
	$(CC) $(CFLAGS) -o $@ guide_sim.c guide_star.c guide_algo.c guide_source.c -lm

clean:
	rm -f guide_sim
.PHONY: clean
