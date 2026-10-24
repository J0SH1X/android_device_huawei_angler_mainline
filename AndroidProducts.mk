#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

PRODUCT_MAKEFILES := \
    aosp_angler_mainline:$(LOCAL_DIR)/aosp_angler_mainline.mk \
    lineage_angler_mainline:$(LOCAL_DIR)/lineage_angler_mainline.mk

$(foreach build_type, user userdebug eng, \
    $(eval COMMON_LUNCH_CHOICES += aosp_angler_mainline-$(build_type)) \
    $(eval COMMON_LUNCH_CHOICES += lineage_angler_mainline-$(build_type)))
