# Cross-compiles psf_bench for the board. Source the toolchain first:
#   cd tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/
#   source env_install_toolchain.sh
CC ?= arm-rockchip830-linux-uclibcgnueabihf-gcc
CFLAGS := -O2 -Wall -Wextra -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard

psf_bench: psf_bench.c
	$(CC) $(CFLAGS) -o $@ $< -lm

clean:
	rm -f psf_bench
.PHONY: clean
