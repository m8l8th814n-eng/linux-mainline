# SPDX-License-Identifier: GPL-2.0

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
M ?= $(shell pwd)

KBUILD_OPTIONS	+= CONFIG_AOC_DP=m

CFLAGS_MODULE	+= -I$(KERNEL_SRC)/../private/google-modules/display/samsung/displayport/dp_audio
EXTRA_SYMBOLS	+= $(OUT_DIR)/../private/google-modules/display/samsung/displayport/Module.symvers
EXTRA_SYMBOLS	+= $(OUT_DIR)/../private/google-modules/aoc/Module.symvers
EXTRA_SYMBOLS	+= $(OUT_DIR)/../private/google-modules/aoc/alsa/Module.symvers

include $(KERNEL_SRC)/../private/google-modules/soc/gs/Makefile.include

modules modules_install clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) W=1 \
	$(KBUILD_OPTIONS) CFLAGS_MODULE="$(CFLAGS_MODULE)" KBUILD_EXTRA_SYMBOLS="$(EXTRA_SYMBOLS)" $(@)
