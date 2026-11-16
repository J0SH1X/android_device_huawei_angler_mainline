#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

ifneq ($(INSTALLED_LK2NDIMAGE_TARGET),)
MKBOOTIMG_LK2ND_IMAGE_PATH := $(INSTALLED_LK2NDIMAGE_TARGET)
else
MKBOOTIMG_LK2ND_IMAGE_PATH := $(DEVICE_PATH)/prebuilts/lk2nd-$(TARGET_LK2ND_PLATFORM).img
endif

include $(DEVICE_PATH)/hyp/hyp.mk

# Layout of the physical boot partition (TARGET_LK2ND_ACTUAL_BOOTIMG_OFFSET = 512K):
#   0..448K lk2nd.img | 448K..512K hyp.img (qhypstub) | 512K.. mkbootimg boot image
TARGET_LK2ND_MAX_IMAGE_SIZE := $(shell expr $(TARGET_LK2ND_ACTUAL_BOOTIMG_OFFSET) - $(TARGET_HYP_IMAGE_SIZE))

# $(1): output image
# $(2): mkbootimg image
# $(3): lk2nd image
# $(4): hyp (qhypstub) image
define build-lk2nd-boot-image
	cp $(3) $(1)
	$(call assert-max-image-size,$(1),$(TARGET_LK2ND_MAX_IMAGE_SIZE))

	lk2nd_size=$$(stat -c%s $(1)); \
	lk2nd_gap=$$(expr $(TARGET_LK2ND_MAX_IMAGE_SIZE) - $$lk2nd_size); \
	dd if=/dev/zero bs=$$lk2nd_gap count=1 >> $(1)

	cat $(4) >> $(1)
	cat $(2) >> $(1)
endef

$(foreach b,$(INSTALLED_BOOTIMAGE_TARGET), $(eval $(call add-dependency,$(b),$(call bootimage-to-kernel,$(b)))))

$(INSTALLED_BOOTIMAGE_TARGET): $(MKBOOTIMG) $(INTERNAL_BOOTIMAGE_FILES) $(BOOTIMAGE_EXTRA_DEPS) $(MKBOOTIMG_LK2ND_IMAGE_PATH) $(INSTALLED_HYPIMAGE_TARGET)
	$(call pretty,"Target boot image with lk2nd: $@")
	$(MKBOOTIMG) --kernel $(call bootimage-to-kernel,$@) $(INTERNAL_BOOTIMAGE_ARGS) $(INTERNAL_MKBOOTIMG_VERSION_ARGS) $(BOARD_MKBOOTIMG_ARGS) --output $@.mkbootimg
	$(call build-lk2nd-boot-image,$@,$@.mkbootimg,$(MKBOOTIMG_LK2ND_IMAGE_PATH),$(INSTALLED_HYPIMAGE_TARGET))
	$(call assert-max-image-size,$@,$(call get-bootimage-partition-size,$@,boot))

$(INSTALLED_RECOVERYIMAGE_TARGET): $(recoveryimage-deps) $(RECOVERYIMAGE_EXTRA_DEPS) $(MKBOOTIMG_LK2ND_IMAGE_PATH) $(INSTALLED_HYPIMAGE_TARGET)
	$(call pretty,"Target recovery image with lk2nd: $@")
	$(call build-recoveryimage-target,$@.mkbootimg,$(recovery_kernel))
	$(call build-lk2nd-boot-image,$@,$@.mkbootimg,$(MKBOOTIMG_LK2ND_IMAGE_PATH),$(INSTALLED_HYPIMAGE_TARGET))
	$(call assert-max-image-size,$@,$(call get-hash-image-max-size,$(BOARD_RECOVERYIMAGE_PARTITION_SIZE)))
