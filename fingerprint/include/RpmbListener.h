/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <android-base/unique_fd.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace angler::fpc {

/**
 * REE side of QSEE listener 0x2000 (RPMB), as served on stock by qseecomd/librpmb (RE).
 * TrustZone builds and authenticates every RPMB frame itself; this only relays them to the eMMC
 * RPMB partition, so a relay bug cannot forge or corrupt authenticated data.
 *
 * Request: u32 cmd, u32 a, u32 b, u32 c, ...
 *   0x101 device info  (a = response version)
 *   0x102 read         a = frames, c = offset of the request frame; out at +0x10
 *   0x103 write        a = frames, c = offset of the data frames;   out (result frame) at +0x10
 *   0x104 partition cfg -> -1 (stock: no /system/etc/rpmb_sec_parti.cfg)
 * Read/write response, in place: a = status, b = output bytes, c = 0x10, data at +0x10.
 *
 * Authentication key programming frames (request type 0x0001, a one-time irreversible eMMC
 * operation) are refused unconditionally.
 */
class RpmbListener {
  public:
    static constexpr uint32_t kListenerId = 0x2000;
    static constexpr size_t kBufSize = 0x5000;

    /** Opens the eMMC RPMB char device (property ro.vendor.fpc.rpmb, else /dev/mmcblk*rpmb). */
    int open();
    void handle(uint8_t* buf, size_t len, size_t* rspLen);

  private:
    int read(const uint8_t* reqFrame, uint32_t frames, uint8_t* out, size_t outCap);
    int write(const uint8_t* frames, uint32_t count, uint8_t* resultOut);
    int devInfo(uint32_t* sizeMult, uint32_t* relSectors);

    android::base::unique_fd mFd;
    std::string mSysfs;  // /sys/block/mmcblkN/device
};

}  // namespace angler::fpc
