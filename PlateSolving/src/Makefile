# Host build and test of the plate solver:
#   make test        synthetic-sky test matrix (see solve_test.c)
# Cross build for the board (source the toolchain first, see Guiding/bench/Makefile):
#   make board       -> solve_bench.arm (cases and catalogue come from solve_test -k/-w)
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu99
XCC     ?= arm-rockchip830-linux-uclibcgnueabihf-gcc
XCFLAGS := -O2 -Wall -Wextra -std=gnu99 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard

SRC := astap_solve.c astap_db.c catalog.c
HDR := astap_solve.h astap_db.h catalog.h

solve_test: solve_test.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ solve_test.c $(SRC) -lm

test: solve_test
	./solve_test

solve_bench.arm: solve_bench.c $(SRC) $(HDR)
	$(XCC) $(XCFLAGS) -o $@ solve_bench.c astap_solve.c astap_db.c -lm

board: solve_bench.arm

clean:
	rm -f solve_test solve_bench.arm
.PHONY: test board clean
