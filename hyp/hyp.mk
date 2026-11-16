#
# hyp.img: the PSCI payload ("qhyp-psci") that lk2nd copies into the Qualcomm
# hypervisor at EL2 (MSM8994 TrustZone has no PSCI). It lives in the 'qhypstub'
# slot, the last 64 KiB of the physical 512 KiB lk2nd area of the boot partition:
#
#   0..448K lk2nd.img | 448K..512K hyp.img | 512K.. Android boot.img
#
# Included from mkbootimg.mk, so every boot/recovery image build produces it.
#

HYP_PATH := $(DEVICE_PATH)/hyp
HYP_OUT_DIR := $(TARGET_OUT_INTERMEDIATES)/HYP_OBJ
INSTALLED_HYPIMAGE_TARGET := $(PRODUCT_OUT)/hyp.img
TARGET_HYP_IMAGE_SIZE := 65536
TARGET_HYP_MAX_PAYLOAD_SIZE := 16384

# System LLVM (like lk2nd's system arm-none-eabi-gcc). Clang 21.1.8 gives the exact
# bytes verified on hardware; the AOSP prebuilt clang (r547379) produces different code.
HYP_CLANG_BIN ?= /usr/bin
HYP_CFLAGS := --target=aarch64-none-elf -Os -ffreestanding -fno-builtin \
    -fno-stack-protector -fno-pic -mgeneral-regs-only -mstrict-align \
    -fno-jump-tables -Wall -Wextra
HYP_SRCS := $(HYP_PATH)/entry.S $(HYP_PATH)/psci.c $(HYP_PATH)/link.ld

$(INSTALLED_HYPIMAGE_TARGET): $(HYP_SRCS)
	$(call pretty,"Target hyp image: $@")
	mkdir -p $(HYP_OUT_DIR)
	$(HYP_CLANG_BIN)/clang $(HYP_CFLAGS) -c $(HYP_PATH)/entry.S -o $(HYP_OUT_DIR)/entry.o
	$(HYP_CLANG_BIN)/clang $(HYP_CFLAGS) -c $(HYP_PATH)/psci.c -o $(HYP_OUT_DIR)/psci.o
	$(HYP_CLANG_BIN)/ld.lld -T $(HYP_PATH)/link.ld -o $(HYP_OUT_DIR)/qhyp-psci.elf \
	    $(HYP_OUT_DIR)/entry.o $(HYP_OUT_DIR)/psci.o
	$(HYP_CLANG_BIN)/llvm-objcopy -O binary -j .text $(HYP_OUT_DIR)/qhyp-psci.elf $(HYP_OUT_DIR)/qhyp-psci.bin
	$(call assert-max-image-size,$(HYP_OUT_DIR)/qhyp-psci.bin,$(TARGET_HYP_MAX_PAYLOAD_SIZE))
	cp $(HYP_OUT_DIR)/qhyp-psci.bin $@
	truncate -s $(TARGET_HYP_IMAGE_SIZE) $@

.PHONY: hyp
hyp: $(INSTALLED_HYPIMAGE_TARGET)

ALL_DEFAULT_INSTALLED_MODULES += $(INSTALLED_HYPIMAGE_TARGET)
