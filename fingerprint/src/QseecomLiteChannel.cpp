/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "fpc-angler-qsl"

#include "QseecomLiteChannel.h"

#include <android-base/logging.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "FpcProtocol.h"
#include "qseecom_lite_uapi.h"

namespace angler::fpc {

namespace {
constexpr const char* kDevice = "/dev/qseecom-lite";
// stock transfer block: 0x40 cmd + 0x40 rsp, cmd u32[0] = shared size, u32[1] = shm phys address
constexpr size_t kCmdLen = 0x40;
constexpr size_t kRspLen = 0x40;
constexpr uint32_t kShmCmdOff = 4;

// secure-world keymaster (normally already loaded by the bootloader; LOAD_APP finds it)
constexpr const char* kKmName = "keymaster";
constexpr size_t kKmBuf = 0x400;
constexpr uint32_t kKmGetAuthTokenKey = 0x205;
constexpr uint32_t kHwAuthFingerprint = 2;  // hw_authenticator_type_t HW_AUTH_FINGERPRINT
}  // namespace

QseecomLiteChannel::QseecomLiteChannel(FsListener& fs, std::string taName)
    : mFs(fs), mName(std::move(taName)) {}

QseecomLiteChannel::~QseecomLiteChannel() { close(); }

int QseecomLiteChannel::open() {
    std::lock_guard<std::mutex> l(mLock);
    if (mLoaded) return 0;

    mDev.reset(::open(kDevice, O_RDWR | O_CLOEXEC));
    if (!mDev.ok()) {
        PLOG(ERROR) << "open " << kDevice;
        return -errno;
    }

    qsl_shm shm = {static_cast<uint32_t>(kShmSize), 0};
    if (ioctl(mDev.get(), QSL_IOC_ALLOC_SHM, &shm) < 0) {
        PLOG(ERROR) << "alloc shm";
        int e = -errno;
        mDev.reset();
        return e;
    }
    void* p = mmap(nullptr, kShmSize, PROT_READ | PROT_WRITE, MAP_SHARED, mDev.get(), 0);
    if (p == MAP_FAILED) {
        PLOG(ERROR) << "mmap shm";
        int e = -errno;
        mDev.reset();
        return e;
    }
    mShm = static_cast<uint8_t*>(p);

    // The TA calls back into the FS listener while loading its db, so it must exist first.
    qsl_listener_reg reg = {FsListener::kListenerId, static_cast<uint32_t>(kListenerBuf)};
    if (ioctl(mDev.get(), QSL_IOC_LISTENER_REGISTER, &reg) < 0) {
        PLOG(ERROR) << "register fs listener";
        int e = -errno;
        munmap(mShm, kShmSize);
        mShm = nullptr;
        mDev.reset();
        return e;
    }
    mStop = false;
    mListener = std::thread(&QseecomLiteChannel::listenerLoop, this);

    // GP file system: the FPC TA stores its template db through it.
    qsl_listener_reg gr = {FsListener::kGpfsListenerId, static_cast<uint32_t>(FsListener::kGpfsBufSize)};
    if (ioctl(mDev.get(), QSL_IOC_LISTENER_REGISTER, &gr) == 0)
        mGpfsThread = std::thread(&QseecomLiteChannel::gpfsLoop, this);
    else
        PLOG(WARNING) << "register GPFS listener";

    // RPMB relay: the QSEE keymaster/gatekeeper keeps its records in RPMB.
    if (mRpmb.open() == 0) {
        qsl_listener_reg rr = {RpmbListener::kListenerId, static_cast<uint32_t>(RpmbListener::kBufSize)};
        if (ioctl(mDev.get(), QSL_IOC_LISTENER_REGISTER, &rr) == 0)
            mRpmbThread = std::thread(&QseecomLiteChannel::rpmbLoop, this);
        else
            PLOG(WARNING) << "register RPMB listener";
    }

    // Debug only: FPC_PROBE_LISTENERS="0x2000,0xb" registers log-only listeners that answer every
    // request with zeros (never touching storage), to see which services an app asks for.
    if (const char* probe = getenv("FPC_PROBE_LISTENERS")) {
        for (const char* p = probe; *p;) {
            char* end = nullptr;
            uint32_t id = static_cast<uint32_t>(strtoul(p, &end, 0));
            if (end == p) break;
            qsl_listener_reg r = {id, 0x10000};
            if (ioctl(mDev.get(), QSL_IOC_LISTENER_REGISTER, &r) == 0) {
                LOG(WARNING) << "probe listener 0x" << std::hex << id << " registered";
                mProbes.emplace_back(&QseecomLiteChannel::probeLoop, this, id);
            } else {
                PLOG(WARNING) << "probe listener register " << id;
            }
            p = *end ? end + 1 : end;
        }
    }

    qsl_load_app app = {};
    strlcpy(app.name, mName.c_str(), sizeof(app.name));
    if (ioctl(mDev.get(), QSL_IOC_LOAD_APP, &app) < 0) {
        PLOG(ERROR) << "load TA " << mName;
        int e = -errno;
        mLock.unlock();
        close();
        mLock.lock();
        return e;
    }
    mAppId = app.app_id;
    mLoaded = true;
    LOG(INFO) << "TA " << mName << " ready, app id " << mAppId;
    return 0;
}

void QseecomLiteChannel::close() {
    std::thread t;
    {
        std::lock_guard<std::mutex> l(mLock);
        mLoaded = false;
        mStop = true;
        if (mDev.ok()) {
            uint32_t id = FsListener::kListenerId;
            ioctl(mDev.get(), QSL_IOC_LISTENER_UNREGISTER, &id);  // wakes the WAIT with ENODEV
            id = RpmbListener::kListenerId;
            ioctl(mDev.get(), QSL_IOC_LISTENER_UNREGISTER, &id);
            id = FsListener::kGpfsListenerId;
            ioctl(mDev.get(), QSL_IOC_LISTENER_UNREGISTER, &id);
        }
        t = std::move(mListener);
    }
    if (t.joinable()) t.join();
    if (mRpmbThread.joinable()) mRpmbThread.join();
    if (mGpfsThread.joinable()) mGpfsThread.join();
    for (auto& p : mProbes) p.detach();  // debug only; blocked in WAIT until the fd closes
    mProbes.clear();
    std::lock_guard<std::mutex> l(mLock);
    if (mShm) munmap(mShm, kShmSize);
    mShm = nullptr;
    mDev.reset();
}

int QseecomLiteChannel::transact(uint8_t* buf, size_t len) {
    std::lock_guard<std::mutex> l(mLock);
    if (!mLoaded) return -ENODEV;
    if (len > kShmSize) return -E2BIG;

    memcpy(mShm, buf, len);

    uint8_t cmd[kCmdLen] = {}, rsp[kRspLen] = {};
    uint32_t sz = static_cast<uint32_t>(len);
    memcpy(cmd, &sz, 4);  // u32[1] is patched by the kernel with the shm physical address

    qsl_send_cmd req = {};
    req.app_id = mAppId;
    req.flags = QSL_SEND_USE_SHM;
    req.shm_cmd_off = kShmCmdOff;
    req.cmd_len = kCmdLen;
    req.cmd_ptr = reinterpret_cast<uintptr_t>(cmd);
    req.rsp_len = kRspLen;
    req.rsp_ptr = reinterpret_cast<uintptr_t>(rsp);
    if (ioctl(mDev.get(), QSL_IOC_SEND_CMD, &req) < 0) {
        PLOG(ERROR) << "send cmd";
        return -errno;
    }
    uint32_t status;
    memcpy(&status, rsp, 4);
    if (status != 0) {
        LOG(ERROR) << "TA response status " << status;
        return -EIO;
    }
    memcpy(buf, mShm, len);
    return 0;
}

int QseecomLiteChannel::getAuthTokenKey(std::vector<uint8_t>* key) {
    std::lock_guard<std::mutex> l(mLock);
    if (!mDev.ok()) return -ENODEV;

    qsl_load_app app = {};
    strlcpy(app.name, kKmName, sizeof(app.name));
    if (ioctl(mDev.get(), QSL_IOC_LOAD_APP, &app) < 0) {
        PLOG(ERROR) << "keymaster TA";
        return -errno;
    }

    // QSEE keymaster: req {u32 cmd = 0x205, u32 hw_authenticator_type} in the first 0x40 bytes,
    // rsp right after it {status, key offset, key len} with the key at rsp + offset.
    uint8_t io[kKmBuf] = {};
    uint8_t* rsp = io + kCmdLen;
    const size_t rspCap = kKmBuf - kCmdLen;
    uint32_t cmd = kKmGetAuthTokenKey;
    uint32_t authType = kHwAuthFingerprint;
    memcpy(io, &cmd, 4);
    memcpy(io + 4, &authType, 4);

    qsl_send_cmd req = {};
    req.app_id = app.app_id;
    req.cmd_len = kCmdLen;
    req.cmd_ptr = reinterpret_cast<uintptr_t>(io);
    req.rsp_len = rspCap;
    req.rsp_ptr = reinterpret_cast<uintptr_t>(rsp);
    int rc = 0;
    if (ioctl(mDev.get(), QSL_IOC_SEND_CMD, &req) < 0) {
        PLOG(ERROR) << "keymaster cmd";
        rc = -errno;
    } else {
        uint32_t status, off, n;
        memcpy(&status, rsp, 4);
        memcpy(&off, rsp + 4, 4);
        memcpy(&n, rsp + 8, 4);
        if (status != 0 || n == 0 || n > kHatKeyMax || off > rspCap || n > rspCap - off) {
            LOG(ERROR) << "bad keymaster response: status " << status << " off " << off
                       << " len " << n;
            rc = -EIO;
        } else {
            key->assign(rsp + off, rsp + off + n);
        }
    }
    memset(io, 0, sizeof(io));
    return rc;
}

int QseecomLiteChannel::keymasterCall(const std::vector<uint8_t>& cmd, size_t rspCap,
                                      std::vector<uint8_t>* rsp) {
    std::lock_guard<std::mutex> l(mLock);
    if (!mDev.ok()) return -ENODEV;
    if (cmd.empty() || cmd.size() > QSL_MAX_BUF || !rspCap || rspCap > QSL_MAX_BUF) return -EINVAL;

    qsl_load_app app = {};
    strlcpy(app.name, kKmName, sizeof(app.name));
    if (ioctl(mDev.get(), QSL_IOC_LOAD_APP, &app) < 0) {
        PLOG(ERROR) << "keymaster TA";
        return -errno;
    }
    std::vector<uint8_t> c(cmd);
    rsp->assign(rspCap, 0);
    qsl_send_cmd req = {};
    req.app_id = app.app_id;
    req.cmd_len = c.size();
    req.cmd_ptr = reinterpret_cast<uintptr_t>(c.data());
    req.rsp_len = rspCap;
    req.rsp_ptr = reinterpret_cast<uintptr_t>(rsp->data());
    int rc = ioctl(mDev.get(), QSL_IOC_SEND_CMD, &req) < 0 ? -errno : 0;
    std::fill(c.begin(), c.end(), 0);
    if (rc) LOG(ERROR) << "keymaster call: " << rc;
    return rc;
}

void QseecomLiteChannel::gpfsLoop() {
    std::vector<uint8_t> buf(FsListener::kGpfsBufSize);
    int fd = mDev.get();
    while (!mStop) {
        qsl_listener_io io = {FsListener::kGpfsListenerId, static_cast<uint32_t>(buf.size()),
                              reinterpret_cast<uintptr_t>(buf.data())};
        if (ioctl(fd, QSL_IOC_LISTENER_WAIT, &io) < 0) {
            if (errno == EINTR) continue;
            if (!mStop) PLOG(ERROR) << "gpfs listener wait";
            return;
        }
        size_t rspLen = 0;
        mFs.handleGpfs(buf.data(), io.len, &rspLen);
        qsl_listener_io out = {FsListener::kGpfsListenerId, static_cast<uint32_t>(rspLen),
                               reinterpret_cast<uintptr_t>(buf.data())};
        if (ioctl(fd, QSL_IOC_LISTENER_RESPOND, &out) < 0) {
            PLOG(ERROR) << "gpfs listener respond";
            return;
        }
    }
}

void QseecomLiteChannel::rpmbLoop() {
    std::vector<uint8_t> buf(RpmbListener::kBufSize);
    int fd = mDev.get();
    while (!mStop) {
        qsl_listener_io io = {RpmbListener::kListenerId, static_cast<uint32_t>(buf.size()),
                              reinterpret_cast<uintptr_t>(buf.data())};
        if (ioctl(fd, QSL_IOC_LISTENER_WAIT, &io) < 0) {
            if (errno == EINTR) continue;
            if (!mStop) PLOG(ERROR) << "rpmb listener wait";
            return;
        }
        size_t rspLen = 0;
        mRpmb.handle(buf.data(), io.len, &rspLen);
        qsl_listener_io out = {RpmbListener::kListenerId, static_cast<uint32_t>(rspLen),
                               reinterpret_cast<uintptr_t>(buf.data())};
        if (ioctl(fd, QSL_IOC_LISTENER_RESPOND, &out) < 0) {
            PLOG(ERROR) << "rpmb listener respond";
            return;
        }
    }
}

void QseecomLiteChannel::probeLoop(uint32_t id) {
    std::vector<uint8_t> buf(0x10000);
    while (!mStop) {
        qsl_listener_io io = {id, static_cast<uint32_t>(buf.size()),
                              reinterpret_cast<uintptr_t>(buf.data())};
        if (ioctl(mDev.get(), QSL_IOC_LISTENER_WAIT, &io) < 0) {
            if (errno == EINTR) continue;
            return;
        }
        std::string hex;
        char tmp[4];
        for (size_t i = 0; i < 0x40 && i < io.len; i++) {
            snprintf(tmp, sizeof(tmp), "%02x", buf[i]);
            hex += tmp;
            if (i % 4 == 3) hex += ' ';
        }
        LOG(WARNING) << "probe listener 0x" << std::hex << id << " request: " << hex;
        std::fill(buf.begin(), buf.begin() + 0x40, 0);
        qsl_listener_io out = {id, 0x40, reinterpret_cast<uintptr_t>(buf.data())};
        ioctl(mDev.get(), QSL_IOC_LISTENER_RESPOND, &out);
    }
}

void QseecomLiteChannel::listenerLoop() {
    std::vector<uint8_t> req(kListenerBuf), rsp(std::max(kListenerBuf, FsListener::kRspBufSize));
    int fd = mDev.get();
    while (!mStop) {
        qsl_listener_io io = {FsListener::kListenerId, static_cast<uint32_t>(req.size()),
                              reinterpret_cast<uintptr_t>(req.data())};
        if (ioctl(fd, QSL_IOC_LISTENER_WAIT, &io) < 0) {
            if (errno == EINTR) continue;
            if (!mStop) PLOG(ERROR) << "listener wait";
            return;
        }
        size_t rspLen = 0;
        mFs.handle(req.data(), io.len, rsp.data(), &rspLen);
        qsl_listener_io out = {FsListener::kListenerId, static_cast<uint32_t>(rspLen),
                               reinterpret_cast<uintptr_t>(rsp.data())};
        if (ioctl(fd, QSL_IOC_LISTENER_RESPOND, &out) < 0) {
            PLOG(ERROR) << "listener respond";
            return;
        }
    }
}

}  // namespace angler::fpc
