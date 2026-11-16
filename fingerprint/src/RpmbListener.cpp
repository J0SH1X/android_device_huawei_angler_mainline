/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "fpc-angler-rpmb"

#include "RpmbListener.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/mmc/ioctl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

namespace angler::fpc {

namespace {
constexpr uint32_t kCmdDevInfo = 0x101;
constexpr uint32_t kCmdRead = 0x102;
constexpr uint32_t kCmdWrite = 0x103;
constexpr uint32_t kCmdPartCfg = 0x104;

constexpr size_t kFrame = 0x200;
constexpr size_t kOutOff = 0x10;
constexpr uint16_t kReqKeyProgramming = 0x0001;
constexpr uint16_t kReqResultRead = 0x0005;
constexpr uint32_t kTypeEmmc = 3;

constexpr uint32_t kMmcWriteMultiple = 25;
constexpr uint32_t kMmcReadMultiple = 18;
constexpr uint32_t kMmcFlagsR1Adtc = 0x35;  // RSP_PRESENT | RSP_CRC | RSP_OPCODE | CMD_ADTC
constexpr uint32_t kReliableWrite = 1u << 31;

uint32_t rd32(const uint8_t* b, size_t off) {
    uint32_t v;
    memcpy(&v, b + off, 4);
    return v;
}
void wr32(uint8_t* b, size_t off, uint32_t v) { memcpy(b + off, &v, 4); }
uint16_t reqType(const uint8_t* frame) { return (frame[0x1fe] << 8) | frame[0x1ff]; }

void setCmd(mmc_ioc_cmd* c, bool write, uint32_t flagExtra, uint32_t opcode, uint32_t blocks,
            void* data) {
    memset(c, 0, sizeof(*c));
    c->write_flag = (write ? 1 : 0) | flagExtra;
    c->opcode = opcode;
    c->flags = kMmcFlagsR1Adtc;
    c->blksz = kFrame;
    c->blocks = blocks;
    mmc_ioc_cmd_set_data((*c), data);
}

std::string findRpmb(std::string* sysfs) {
    std::string forced = android::base::GetProperty("ro.vendor.fpc.rpmb", "");
    std::string dev;
    if (!forced.empty()) {
        dev = forced;
    } else if (DIR* d = opendir("/dev")) {
        while (dirent* e = readdir(d)) {
            if (android::base::StartsWith(e->d_name, "mmcblk") &&
                android::base::EndsWith(e->d_name, "rpmb")) {
                dev = std::string("/dev/") + e->d_name;
                break;
            }
        }
        closedir(d);
    }
    if (!dev.empty()) {
        std::string base = dev.substr(5, dev.size() - 5 - 4);  // mmcblkN
        *sysfs = "/sys/block/" + base + "/device";
    }
    return dev;
}
}  // namespace

int RpmbListener::open() {
    std::string dev = findRpmb(&mSysfs);
    if (dev.empty()) {
        LOG(ERROR) << "no eMMC RPMB device";
        return -ENODEV;
    }
    mFd.reset(::open(dev.c_str(), O_RDWR | O_CLOEXEC));
    if (!mFd.ok()) {
        PLOG(ERROR) << "open " << dev;
        return -errno;
    }
    LOG(INFO) << "RPMB relay on " << dev;
    return 0;
}

int RpmbListener::devInfo(uint32_t* sizeMult, uint32_t* relSectors) {
    std::string a, b;
    if (!android::base::ReadFileToString(mSysfs + "/raw_rpmb_size_mult", &a) ||
        !android::base::ReadFileToString(mSysfs + "/rel_sectors", &b))
        return -ENOENT;
    *sizeMult = static_cast<uint32_t>(strtoul(a.c_str(), nullptr, 0));
    *relSectors = static_cast<uint32_t>(strtoul(b.c_str(), nullptr, 0));
    return (*sizeMult && *relSectors) ? 0 : -EINVAL;
}

int RpmbListener::read(const uint8_t* reqFrame, uint32_t frames, uint8_t* out, size_t outCap) {
    if (!frames || frames * kFrame > outCap) return -EINVAL;
    std::vector<uint8_t> req(reqFrame, reqFrame + kFrame);  // input may overlap the output
    std::vector<uint8_t> data(frames * kFrame);
    std::vector<uint8_t> mc(sizeof(mmc_ioc_multi_cmd) + 2 * sizeof(mmc_ioc_cmd));
    auto* m = reinterpret_cast<mmc_ioc_multi_cmd*>(mc.data());
    m->num_of_cmds = 2;
    setCmd(&m->cmds[0], true, 0, kMmcWriteMultiple, 1, req.data());
    setCmd(&m->cmds[1], false, 0, kMmcReadMultiple, frames, data.data());
    if (ioctl(mFd.get(), MMC_IOC_MULTI_CMD, m) < 0) {
        PLOG(ERROR) << "RPMB read";
        return -errno;
    }
    memcpy(out, data.data(), data.size());
    return 0;
}

int RpmbListener::write(const uint8_t* frames, uint32_t count, uint8_t* resultOut) {
    for (uint32_t i = 0; i < count; i++) {
        if (reqType(frames + i * kFrame) == kReqKeyProgramming) {
            LOG(ERROR) << "refusing RPMB authentication key programming";
            return -EPERM;
        }
    }
    std::vector<uint8_t> resultReq(kFrame, 0);
    resultReq[0x1fe] = kReqResultRead >> 8;
    resultReq[0x1ff] = kReqResultRead & 0xff;
    std::vector<uint8_t> frame(kFrame), result(kFrame);
    std::vector<uint8_t> mc(sizeof(mmc_ioc_multi_cmd) + 3 * sizeof(mmc_ioc_cmd));
    auto* m = reinterpret_cast<mmc_ioc_multi_cmd*>(mc.data());

    // One frame per transaction with a result read after each, like stock librpmb.
    for (uint32_t i = 0; i < count; i++) {
        memcpy(frame.data(), frames + i * kFrame, kFrame);
        memset(mc.data(), 0, mc.size());
        m->num_of_cmds = 3;
        setCmd(&m->cmds[0], true, kReliableWrite, kMmcWriteMultiple, 1, frame.data());
        setCmd(&m->cmds[1], true, 0, kMmcWriteMultiple, 1, resultReq.data());
        setCmd(&m->cmds[2], false, 0, kMmcReadMultiple, 1, result.data());
        if (ioctl(mFd.get(), MMC_IOC_MULTI_CMD, m) < 0) {
            PLOG(ERROR) << "RPMB write";
            return -errno;
        }
        memcpy(resultOut, result.data(), kFrame);
        if (result[0x1fc] == 0x80) {
            LOG(ERROR) << "RPMB write counter expired";
            return -EIO;
        }
        if (result[0x1fd] != 0) {
            LOG(ERROR) << "RPMB operation result " << int(result[0x1fd]);
            return -EIO;
        }
    }
    return 0;
}

void RpmbListener::handle(uint8_t* buf, size_t len, size_t* rspLen) {
    *rspLen = 0x10;
    if (len < 0x10) return;
    const uint32_t cmd = rd32(buf, 0);
    const uint32_t a = rd32(buf, 4), c = rd32(buf, 0xc);

    switch (cmd) {
        case kCmdDevInfo: {
            uint32_t mult = 0, rel = 0;
            int rc = mFd.ok() ? devInfo(&mult, &rel) : -ENODEV;
            uint8_t out[0x28] = {};
            wr32(out, 0, cmd);
            if (a >= 2) {  // stock v2 layout
                wr32(out, 4, 2);
                wr32(out, 8, rc ? 0xffffffff : 0);
                wr32(out, 0xc, mult);
                wr32(out, 0x10, rel);
                wr32(out, 0x14, kTypeEmmc);
                *rspLen = 0x28;
            } else {
                wr32(out, 4, 1);
                wr32(out, 8, (rc || a < 1) ? 0xffffffff : 0);
                wr32(out, 0xc, a < 1 ? 0 : mult);
                wr32(out, 0x10, a < 1 ? 0 : rel);
                *rspLen = 0x14;
            }
            memcpy(buf, out, *rspLen);
            LOG(INFO) << "RPMB dev info: mult " << mult << " rel " << rel << " rc " << rc;
            return;
        }
        case kCmdRead:
        case kCmdWrite: {
            int rc = -EINVAL;
            uint32_t outLen = 0;
            const uint64_t need = static_cast<uint64_t>(c) + uint64_t(a) * kFrame;
            if (mFd.ok() && a > 0 && c >= 0x10 && need <= len && kOutOff + kFrame <= len) {
                if (cmd == kCmdRead) {
                    rc = read(buf + c, a, buf + kOutOff, len - kOutOff);
                    outLen = a * kFrame;
                } else {
                    std::vector<uint8_t> frames(buf + c, buf + c + a * kFrame);
                    rc = write(frames.data(), a, buf + kOutOff);
                    outLen = kFrame;
                }
            }
            LOG(INFO) << (cmd == kCmdRead ? "RPMB read " : "RPMB write ") << a << " frames: " << rc;
            wr32(buf, 4, rc ? 0xffffffff : 0);
            wr32(buf, 8, outLen);
            wr32(buf, 0xc, kOutOff);
            *rspLen = std::min<size_t>(len, kOutOff + outLen);
            return;
        }
        case kCmdPartCfg:
            wr32(buf, 4, 0xffffffff);
            wr32(buf, 8, 0);
            wr32(buf, 0xc, 0x10);
            return;
        default:
            LOG(WARNING) << "unknown RPMB listener command 0x" << std::hex << cmd;
            wr32(buf, 4, 0xfffffff9);
            wr32(buf, 8, 0);
            wr32(buf, 0xc, 0x10);
            return;
    }
}

}  // namespace angler::fpc
