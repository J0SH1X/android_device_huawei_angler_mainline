#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

DEVICE_PATH := device/huawei/angler_mainline
PRODUCT_USE_DYNAMIC_PARTITIONS := true


# Inherit options from mainline/qcom-common
TARGET_QCOM_SOC := msm8994
## TODO: Bringup the corresponding hardware and remove the following definitions
TARGET_AUDIO_HAL := default-aidl
# own primary module policy (earpiece + hardware voice routes), see audio/
TARGET_AUDIO_POLICY := none
TARGET_SUPPORTS_SUSPEND := false
# Sensors: mainline_ext HAL with the nanohub backend (+ IIO); no SSC/libssc on msm8994
TARGET_SENSORS_HAL := mainline_ext
include device/mainline/qcom-common/optional/options.mk

# Inherit from mainline/qcom-common
$(call inherit-product, device/mainline/qcom-common/mainline_qcom-common.mk)

# Sensors: runtime backend list (soong defaults are overridden in BoardConfig.mk, which is
# evaluated after the inherited qcom-common-ext product makefiles)
PRODUCT_VENDOR_PROPERTIES += vendor.sensors.backends=nanohub,iio

# AAPT
PRODUCT_AAPT_PREF_CONFIG := mdpi

# Audio
PRODUCT_PACKAGES += \
    audio.angler_mainline.xml \
    audio_voice_paths.conf \
    primary_audio_policy_configuration.angler_mainline.xml

PRODUCT_COPY_FILES += \
    device/google/cuttlefish/shared/config/audio/policy/audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/audio_policy_volumes.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_volumes.xml \
    frameworks/av/services/audiopolicy/config/bluetooth_audio_policy_configuration_7_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/bluetooth_audio_policy_configuration_7_0.xml \
    frameworks/av/services/audiopolicy/config/bluetooth_with_le_audio_policy_configuration_7_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/bluetooth_with_le_audio_policy_configuration_7_0.xml \
    frameworks/av/services/audiopolicy/config/default_volume_tables.xml:$(TARGET_COPY_OUT_VENDOR)/etc/default_volume_tables.xml \
    frameworks/av/services/audiopolicy/config/r_submix_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/r_submix_audio_policy_configuration.xml

# Bootanimation
TARGET_BOOTANIMATION_HALF_RES := true
TARGET_SCREEN_HEIGHT := 2560
TARGET_SCREEN_WIDTH := 1440

# Dalvik heap
$(call inherit-product, frameworks/native/build/phone-xhdpi-2048-dalvik-heap.mk)

# HIDL
PRODUCT_PACKAGES += \
    vndservicemanager

# Init
PRODUCT_PACKAGES += \
    init.angler.rc \
    init.recovery.angler.rc \
    ueventd.angler.rc

PRODUCT_PACKAGES += \
    use_memfd.rc \
    zram.rc

$(call soong_config_set,mainline_common_libinit,set_properties_from,devicetree)

# Kernel
PRODUCT_OTA_ENFORCE_VINTF_KERNEL_REQUIREMENTS := false

PRODUCT_PACKAGES += \
    modules.load.normal

# Overlays
DEVICE_PACKAGE_OVERLAYS += \
    $(DEVICE_PATH)/overlays/overlay

PRODUCT_PACKAGES += \
    AodDefaultOnOverlay

# Permissions
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.touchscreen.multitouch.jazzhand.xml:$(TARGET_COPY_OUT_ODM)/etc/permissions/android.hardware.touchscreen.multitouch.jazzhand.xml

# Scoped Storage
$(call inherit-product, $(SRC_TARGET_DIR)/product/emulated_storage.mk)

# Shipping API level
PRODUCT_SHIPPING_API_LEVEL := 33

# Soong namespaces
PRODUCT_SOONG_NAMESPACES += \
    $(DEVICE_PATH) \
    kernel/mainline/configs

PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/fstab/fstab.angler:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.angler \
    $(DEVICE_PATH)/fstab/fstab.angler:$(TARGET_COPY_OUT)/ramdisk/fstab.angler

# Firmware
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/firmware/adsp.mbn:$(TARGET_COPY_OUT_ODM)/firmware/adsp.mbn \
    $(DEVICE_PATH)/firmware/cpe.mbn:$(TARGET_COPY_OUT_ODM)/firmware/cpe.mbn \
    $(DEVICE_PATH)/firmware/fingerpr.mbn:$(TARGET_COPY_OUT_ODM)/firmware/fingerpr.mbn \
    $(DEVICE_PATH)/firmware/venus.mbn:$(TARGET_COPY_OUT_ODM)/firmware/venus.mbn \
    $(DEVICE_PATH)/firmware/widevine.mbn:$(TARGET_COPY_OUT_ODM)/firmware/widevine.mbn \
    $(DEVICE_PATH)/firmware/adsp.mbn:$(TARGET_COPY_OUT_ODM)/firmware/qcadsp8994.mbn

# BCM4358 Wi-Fi (brcmfmac, stock bcmdhd.cal as NVRAM) + Bluetooth patchram
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/firmware/brcm/BCM.hcd:$(TARGET_COPY_OUT_ODM)/firmware/brcm/BCM.hcd \
    $(DEVICE_PATH)/firmware/brcm/BCM4358A3.hcd:$(TARGET_COPY_OUT_ODM)/firmware/brcm/BCM4358A3.hcd \
    $(DEVICE_PATH)/firmware/brcm/brcmfmac4358-pcie.bin:$(TARGET_COPY_OUT_ODM)/firmware/brcm/brcmfmac4358-pcie.bin \
    $(DEVICE_PATH)/firmware/brcm/brcmfmac4358-pcie.txt:$(TARGET_COPY_OUT_ODM)/firmware/brcm/brcmfmac4358-pcie.txt

# Firmware for the kernel's direct loader (firmware_class.path=/vendor/firmware/).
# Anything that misses this path goes through ueventd's sysfs fallback, which is serialized and
# times out (60 s) on every missing board-specific name (brcmfmac/btbcm ask for
# "<name>.huawei,angler.*" first) -> minutes of stalled boot. The comma is only in the destination.
LFW := external/linux-firmware-mainline
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/firmware/adsp.mbn:$(TARGET_COPY_OUT_VENDOR)/firmware/qcadsp8994.mbn \
    $(DEVICE_PATH)/firmware/brcm/brcmfmac4358-pcie.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/brcm/brcmfmac4358-pcie.bin \
    $(DEVICE_PATH)/firmware/brcm/brcmfmac4358-pcie.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/brcm/brcmfmac4358-pcie.huawei,angler.bin \
    $(DEVICE_PATH)/firmware/brcm/brcmfmac4358-pcie.txt:$(TARGET_COPY_OUT_VENDOR)/firmware/brcm/brcmfmac4358-pcie.txt \
    $(DEVICE_PATH)/firmware/brcm/brcmfmac4358-pcie.txt:$(TARGET_COPY_OUT_VENDOR)/firmware/brcm/brcmfmac4358-pcie.huawei,angler.txt \
    $(DEVICE_PATH)/firmware/brcm/BCM.hcd:$(TARGET_COPY_OUT_VENDOR)/firmware/brcm/BCM.hcd \
    $(DEVICE_PATH)/firmware/brcm/BCM.hcd:$(TARGET_COPY_OUT_VENDOR)/firmware/brcm/BCM.huawei,angler.hcd \
    $(DEVICE_PATH)/firmware/brcm/BCM4358A3.hcd:$(TARGET_COPY_OUT_VENDOR)/firmware/brcm/BCM4358A3.hcd \
    $(DEVICE_PATH)/firmware/brcm/BCM4358A3.hcd:$(TARGET_COPY_OUT_VENDOR)/firmware/brcm/BCM4358A3.huawei,angler.hcd \
    $(DEVICE_PATH)/firmware/fingerpr.mbn:$(TARGET_COPY_OUT_VENDOR)/firmware/fingerpr.mbn

# Modem firmware symlinks (see Android.bp)
PRODUCT_PACKAGES += \
    angler_modem_fw_mba_b00 \
    angler_modem_fw_mba_mdt \
    angler_modem_fw_modem_b00 \
    angler_modem_fw_modem_b01 \
    angler_modem_fw_modem_b03 \
    angler_modem_fw_modem_b04 \
    angler_modem_fw_modem_b05 \
    angler_modem_fw_modem_b06 \
    angler_modem_fw_modem_b07 \
    angler_modem_fw_modem_b08 \
    angler_modem_fw_modem_b09 \
    angler_modem_fw_modem_b10 \
    angler_modem_fw_modem_b11 \
    angler_modem_fw_modem_b12 \
    angler_modem_fw_modem_b13 \
    angler_modem_fw_modem_b14 \
    angler_modem_fw_modem_b18 \
    angler_modem_fw_modem_b19 \
    angler_modem_fw_modem_b20 \
    angler_modem_fw_modem_b22 \
    angler_modem_fw_modem_mdt

# Telephony: QMI radio HAL (radio/), qmicli for debugging
PRODUCT_PACKAGES += \
    android.hardware.radio-service.qmi \
    qmicli

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.telephony.gsm.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.gsm.xml

PRODUCT_VENDOR_PROPERTIES += \
    ro.telephony.default_network=9

# Fingerprint: FPC1020 via the QSEE fingerpr TA (fingerprint/README.md)
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/fingerprint/uinput-fpc.kl:$(TARGET_COPY_OUT_VENDOR)/usr/keylayout/uinput-fpc.kl

PRODUCT_PACKAGES += \
    android.hardware.biometrics.fingerprint-service.angler

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.fingerprint.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.fingerprint.xml
