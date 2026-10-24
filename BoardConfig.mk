#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

USES_DEVICE_HUAWEI_ANGLER := true
TARGET_INITIAL_BRINGUP := true

# Inherit from mainline/qcom-common
include device/mainline/qcom-common/BoardConfigMainlineQcomCommon.mk

# A/B
AB_OTA_UPDATER := false

# Bootloader
BOARD_BOOT_HEADER_VERSION := 2
#BOARD_CUSTOM_BOOTIMG := true
#BOARD_CUSTOM_BOOTIMG_MK := $(DEVICE_PATH)/mkbootimg.mk
BOARD_MKBOOTIMG_ARGS += --header_version $(BOARD_BOOT_HEADER_VERSION)
#TARGET_LK2ND_MAKE_FLAGS := \
#    LK2ND_ADTBS=msm8994-huawei-angler.dtb \
#    LK2ND_COMPATIBLE=huawei,angler \
#    OSVERSION_IN_BOOTIMAGE=1

# Boot parameters
BOARD_KERNEL_CMDLINE := \
    $(MAINLINE_COMMON_ANDROIDBOOT_PARAMS) \
    $(MAINLINE_COMMON_KERNEL_PARAMS) \
    $(MAINLINE_QCOM_KERNEL_PARAMS) \
    $(MAINLINE_QCOM_SOC_ANDROIDBOOT_PARAMS) \
    androidboot.hardware=angler \
    androidboot.verifiedbootstate=orange \
    console=tty0
    androidboot.super_partition=system

BOARD_KERNEL_CMDLINE += \
    androidboot.selinux=permissive \
    audit=0 \
    lk2nd.spin-table=force

# Display
TARGET_SCREEN_DENSITY := 518

# Filesystem
TARGET_USERIMAGES_USE_F2FS := true
TARGET_USERIMAGES_USE_EXT4 := true

# Kernel
BOARD_INCLUDE_DTB_IN_BOOTIMG := true
TARGET_KERNEL_SOURCE := kernel/mainline/msm8994-mainline

TARGET_DTB_LIST_WILDCARD := \
    qcom/msm8994-huawei-angler-rev-101 \

TARGET_KERNEL_CONFIG_EXT := \
    $(DEVICE_PATH)/kconfigs/mmo_android_defconfig

# Kernel modules
BOARD_RECOVERY_RAMDISK_KERNEL_MODULES_LOAD := \
    $(strip $(shell cat $(DEVICE_PATH)/modprobe/modules.load.basic)) \
    $(strip $(shell cat $(DEVICE_PATH)/modprobe/modules.load.drm)) \
    $(strip $(shell cat $(DEVICE_PATH)/modprobe/modules.load.touchscreen))
BOARD_VENDOR_KERNEL_MODULES_LOAD := \
    $(BOARD_RECOVERY_RAMDISK_KERNEL_MODULES_LOAD)
RECOVERY_KERNEL_MODULES := \
    $(strip $(shell cat $(DEVICE_PATH)/modprobe/modules.include_dep.basic)) \
    $(strip $(shell cat $(DEVICE_PATH)/modprobe/modules.include_dep.drm)) \
    $(strip $(shell cat $(DEVICE_PATH)/modprobe/modules.include_dep.touchscreen)) \
    $(BOARD_RECOVERY_RAMDISK_KERNEL_MODULES_LOAD)

# OTA
TARGET_OTA_ASSERT_DEVICE := angler,angler_mainline

# Partitions
BOARD_BOOTIMAGE_PARTITION_SIZE := 32768000
BOARD_CACHEIMAGE_PARTITION_SIZE := 102400000
BOARD_CACHEIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_RECOVERYIMAGE_PARTITION_SIZE := 32768000

BOARD_SUPER_PARTITION_METADATA_DEVICE := system
BOARD_SUPER_PARTITION_BLOCK_DEVICES := system

BOARD_SUPER_PARTITION_SIZE := 3145728000
BOARD_SUPER_PARTITION_SYSTEM_DEVICE_SIZE := 3145728000

BOARD_VENDORIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_SYSTEM_EXTIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_PRODUCTIMAGE_FILE_SYSTEM_TYPE := ext4

BOARD_USES_METADATA_PARTITION := true
TARGET_COPY_OUT_VENDOR := vendor
TARGET_COPY_OUT_PRODUCT := product
TARGET_COPY_OUT_SYSTEM_EXT := system_ext
TARGET_COPY_OUT_SYSTEM := system

BOARD_SUPER_PARTITION_GROUPS := qcom_dynamic_partitions
BOARD_QCOM_DYNAMIC_PARTITIONS_SIZE := 3141533696
BOARD_QCOM_DYNAMIC_PARTITIONS_PARTITION_LIST := system vendor product system_ext

# Properties
TARGET_VENDOR_PROP += $(DEVICE_PATH)/properties/vendor.prop

# Ramdisk
BOARD_RAMDISK_USE_LZ4 := true

# Recovery
$(INSTALLED_RECOVERYIMAGE_TARGET): $(INSTALLED_DTBIMAGE_TARGET)

# pass it to mkbootimg for recovery
BOARD_RECOVERY_MKBOOTIMG_ARGS += --header_version 2 --dtb $(INSTALLED_DTBIMAGE_TARGET)
TARGET_RECOVERY_FSTAB := $(DEVICE_PATH)/fstab/fstab.angler

# SELinux
BOARD_ODM_SEPOLICY_DIRS += \
    $(DEVICE_PATH)/sepolicy/odm

# VINTF
DEVICE_MANIFEST_FILE := \
    $(DEVICE_PATH)/vintf/manifest.xml
