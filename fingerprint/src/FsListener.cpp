/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "fpc-angler-fs"

#include "FsListener.h"

#include <android-base/logging.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>

namespace angler::fpc {

namespace {

enum : uint32_t {
    kOpen = 0x202,
    kCreat = 0x206,
    kRead = 0x207,
    kWrite = 0x208,
    kClose = 0x209,
    kLseek = 0x20a,
    kLink = 0x20b,
    kUnlink = 0x20c,
    kRmdirR = 0x20d,
    kFstat = 0x20e,
    kLstat = 0x20f,
    kMkdir = 0x210,
    kTestDir = 0x211,
    kRemove = 0x213,
    kFsync = 0x216,
    kRename = 0x217,
    kStatfs = 0x218,
    kLastErrno = 0x21c,
    kExitThread = 0x21d,
};

// Response: u32 cmd @0, i32 ret @4, data @8.
constexpr size_t kRspRet = 4;
constexpr size_t kRspData = 8;
constexpr size_t kStatSize = 0x68;  // 32-bit ARM struct stat64, what the TA was built against

uint32_t rd32(const uint8_t* b, size_t off) {
    uint32_t v;
    memcpy(&v, b + off, 4);
    return v;
}
void wr32(uint8_t* b, size_t off, uint32_t v) { memcpy(b + off, &v, 4); }
void wr64(uint8_t* b, size_t off, uint64_t v) { memcpy(b + off, &v, 8); }

bool underRoot(const std::string& path, const std::string& root) {
    if (root.empty() || path.size() < root.size()) return false;
    if (path.compare(0, root.size(), root) != 0) return false;
    return path.size() == root.size() || path[root.size()] == '/' || root.back() == '/';
}

// Fills the bionic arm32 stat64 layout.
void packStat(const struct stat& st, uint8_t* o) {
    memset(o, 0, kStatSize);
    wr64(o, 0x00, st.st_dev);
    wr32(o, 0x0c, static_cast<uint32_t>(st.st_ino));
    wr32(o, 0x10, st.st_mode);
    wr32(o, 0x14, st.st_nlink);
    wr32(o, 0x18, st.st_uid);
    wr32(o, 0x1c, st.st_gid);
    wr64(o, 0x20, st.st_rdev);
    wr64(o, 0x30, static_cast<uint64_t>(st.st_size));
    wr32(o, 0x38, static_cast<uint32_t>(st.st_blksize));
    wr64(o, 0x40, static_cast<uint64_t>(st.st_blocks));
    wr32(o, 0x48, static_cast<uint32_t>(st.st_atim.tv_sec));
    wr32(o, 0x4c, static_cast<uint32_t>(st.st_atim.tv_nsec));
    wr32(o, 0x50, static_cast<uint32_t>(st.st_mtim.tv_sec));
    wr32(o, 0x54, static_cast<uint32_t>(st.st_mtim.tv_nsec));
    wr32(o, 0x58, static_cast<uint32_t>(st.st_ctim.tv_sec));
    wr32(o, 0x5c, static_cast<uint32_t>(st.st_ctim.tv_nsec));
    wr64(o, 0x60, st.st_ino);
}

}  // namespace

FsListener::FsListener(std::string writableRoot, std::vector<std::string> readOnlyRoots)
    : mWritableRoot(std::move(writableRoot)), mReadOnlyRoots(std::move(readOnlyRoots)) {
    while (mWritableRoot.size() > 1 && mWritableRoot.back() == '/') mWritableRoot.pop_back();
}

FsListener::~FsListener() {
    for (int fd : mFds) close(fd);
}

bool FsListener::resolve(const uint8_t* req, size_t reqLen, size_t off, bool write,
                         std::string* out) {
    if (off + kPathLen > reqLen) return false;
    const char* p = reinterpret_cast<const char*>(req + off);
    size_t n = strnlen(p, kPathLen);
    if (n == kPathLen || n == 0) return false;  // not NUL terminated / empty
    std::string path(p, n);
    if (path[0] != '/') return false;
    // reject ".." components and double slashes' tricks outright
    for (size_t i = 0; i < path.size();) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        if (path.compare(i, j - i, "..") == 0) return false;
        i = j + 1;
    }
    bool ok = underRoot(path, mWritableRoot);
    if (!ok && !write) {
        for (const auto& r : mReadOnlyRoots) ok = ok || underRoot(path, r);
    }
    if (!ok) {
        LOG(WARNING) << "path refused: " << path;
        return false;
    }
    *out = std::move(path);
    return true;
}

void FsListener::handle(const uint8_t* req, size_t reqLen, uint8_t* rsp, size_t* rspLen) {
    memset(rsp, 0, kRspBufSize);
    *rspLen = kRspData;
    if (reqLen < 8) {
        wr32(rsp, kRspRet, static_cast<uint32_t>(-EINVAL));
        return;
    }
    const uint32_t cmd = rd32(req, 0);
    wr32(rsp, 0, cmd);

    int ret = 0;
    auto fail = [&](int e) {
        mLastErrno = e;
        ret = -e;
    };
    std::string path, path2;

    switch (cmd) {
        case kOpen:
        case kCreat: {
            // open: path@4, flags@0x104   creat: path@4, mode@0x104
            // Parent directories are created, as the stock listener does (mkdir -p).
            bool isCreat = cmd == kCreat;
            if (reqLen < 0x108 || !resolve(req, reqLen, 4, true, &path)) { fail(EACCES); break; }
            int flags = isCreat ? (O_WRONLY | O_CREAT | O_TRUNC) : static_cast<int>(rd32(req, 0x104));
            mode_t mode = isCreat ? (rd32(req, 0x104) & 0600) : 0600;
            bool writing = (flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC));
            if (writing) {
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
            }
            int fd = open(path.c_str(), flags | O_CLOEXEC | O_NOFOLLOW, mode);
            if (fd < 0) { fail(errno); break; }
            mFds.insert(fd);
            ret = fd;
            break;
        }
        case kRead: {  // fd@4, n@8 ; data at rsp+8
            int fd = static_cast<int>(rd32(req, 4));
            size_t n = std::min<size_t>(rd32(req, 8), kMaxIo);
            if (!tracked(fd)) { fail(EBADF); break; }
            ssize_t r = read(fd, rsp + kRspData, n);
            if (r < 0) { fail(errno); break; }
            ret = static_cast<int>(r);
            *rspLen = kRspData + r;
            break;
        }
        case kWrite: {  // ASSUMED layout: fd@4, n@8, data@0xc
            int fd = static_cast<int>(rd32(req, 4));
            size_t n = rd32(req, 8);
            if (!tracked(fd)) { fail(EBADF); break; }
            if (n > kMaxIo || 0xc + n > reqLen) { fail(EINVAL); break; }
            ssize_t r = write(fd, req + 0xc, n);
            if (r < 0) { fail(errno); break; }
            ret = static_cast<int>(r);
            break;
        }
        case kClose: {
            int fd = static_cast<int>(rd32(req, 4));
            if (!tracked(fd)) { fail(EBADF); break; }
            mFds.erase(fd);
            if (close(fd) < 0) fail(errno);
            break;
        }
        case kLseek: {  // fd@4, off@8 (int), whence@0xc
            int fd = static_cast<int>(rd32(req, 4));
            if (!tracked(fd)) { fail(EBADF); break; }
            off_t r = lseek(fd, static_cast<int32_t>(rd32(req, 8)), static_cast<int>(rd32(req, 0xc)));
            if (r < 0) fail(errno); else ret = static_cast<int>(r);
            break;
        }
        case kFsync: {
            int fd = static_cast<int>(rd32(req, 4));
            if (!tracked(fd)) { fail(EBADF); break; }
            if (fsync(fd) < 0) fail(errno);
            break;
        }
        case kFstat:
        case kLstat: {
            struct stat st;
            int r;
            if (cmd == kFstat) {
                int fd = static_cast<int>(rd32(req, 4));
                if (!tracked(fd)) { fail(EBADF); break; }
                r = fstat(fd, &st);
            } else {
                if (!resolve(req, reqLen, 4, false, &path)) { fail(EACCES); break; }
                r = lstat(path.c_str(), &st);
            }
            if (r < 0) { fail(errno); break; }
            packStat(st, rsp + kRspData);
            *rspLen = kRspData + kStatSize;
            break;
        }
        case kUnlink:
        case kRemove:
        case kRmdirR: {
            if (!resolve(req, reqLen, 4, true, &path) || path == mWritableRoot) { fail(EACCES); break; }
            std::error_code ec;
            if (cmd == kUnlink) {
                if (unlink(path.c_str()) < 0) fail(errno);
            } else {
                std::filesystem::remove_all(path, ec);
                if (ec) fail(ec.value());
            }
            break;
        }
        case kMkdir: {  // path@4
            if (!resolve(req, reqLen, 4, true, &path)) { fail(EACCES); break; }
            std::error_code ec;
            std::filesystem::create_directories(path, ec);
            if (ec) { fail(ec.value()); break; }
            chmod(path.c_str(), 0700);
            break;
        }
        case kTestDir: {
            if (!resolve(req, reqLen, 4, false, &path)) { fail(EACCES); break; }
            struct stat st;
            ret = (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
            break;
        }
        case kRename:
        case kLink: {  // old@4, new@0x104
            if (reqLen < 0x208 || !resolve(req, reqLen, 4, true, &path) ||
                !resolve(req, reqLen, 0x104, true, &path2)) { fail(EACCES); break; }
            int r = cmd == kRename ? rename(path.c_str(), path2.c_str()) : link(path.c_str(), path2.c_str());
            if (r < 0) fail(errno);
            break;
        }
        case kStatfs: {  // ASSUMED: rsp data = u64 free bytes
            struct statfs sf;
            if (statfs(mWritableRoot.c_str(), &sf) < 0) { fail(errno); break; }
            wr64(rsp, kRspData, static_cast<uint64_t>(sf.f_bavail) * sf.f_bsize);
            *rspLen = kRspData + 8;
            break;
        }
        case kLastErrno:
            ret = mLastErrno;
            break;
        case kExitThread:
            break;
        default:
            LOG(WARNING) << "unsupported fs listener command 0x" << std::hex << cmd;
            fail(ENOSYS);
            break;
    }
    wr32(rsp, kRspRet, static_cast<uint32_t>(ret));
    if (android::base::GetMinimumLogSeverity() <= android::base::DEBUG || ret < 0)
        LOG(ret < 0 ? WARNING : DEBUG) << "fs 0x" << std::hex << cmd << std::dec << " " << path
                                       << " -> " << ret;
}

void FsListener::handleGpfs(uint8_t* buf, size_t len, size_t* rspLen) {
    constexpr size_t kPathOff = 4, kOffOff = 0x104, kSizeOff = 0x108, kDataOff = 0x10c;
    constexpr size_t kReadDataOff = 0xc;
    *rspLen = 8;
    if (len < kDataOff) {
        wr32(buf, 4, static_cast<uint32_t>(-EINVAL));
        return;
    }
    const uint32_t op = rd32(buf, 0);
    const uint32_t off = rd32(buf, kOffOff);
    const uint32_t size = rd32(buf, kSizeOff);
    std::string path;
    int status = 0;
    uint32_t bytes = 0;

    if (!resolve(buf, len, kPathOff, op != 0, &path)) {
        status = -EACCES;
    } else if (op == 0) {  // read
        if (size > len - kReadDataOff) {
            status = -EINVAL;
        } else {
            int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0) {
                status = -errno;
            } else {
                ssize_t r = pread(fd, buf + kReadDataOff, size, off);
                status = r < 0 ? -errno : 0;
                bytes = r < 0 ? 0 : static_cast<uint32_t>(r);
                close(fd);
            }
        }
        *rspLen = kReadDataOff + bytes;
    } else if (op == 1) {  // write
        if (size > len - kDataOff) {
            status = -EINVAL;
        } else {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
            int fd = open(path.c_str(),
                          O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | (off == 0 ? O_TRUNC : 0),
                          0600);
            if (fd < 0) {
                status = -errno;
            } else {
                ssize_t w = pwrite(fd, buf + kDataOff, size, off);
                status = w < 0 ? -errno : (fsync(fd) < 0 ? -errno : 0);
                bytes = w < 0 ? 0 : static_cast<uint32_t>(w);
                close(fd);
            }
        }
        *rspLen = 0xc;
    } else if (op == 2) {  // remove
        if (unlink(path.c_str()) < 0 && errno != ENOENT) status = -errno;
    } else {
        status = -ENOSYS;
    }

    wr32(buf, 0, op);
    wr32(buf, 4, static_cast<uint32_t>(status));
    if (*rspLen >= 0xc) wr32(buf, 8, bytes);
    if (android::base::GetMinimumLogSeverity() <= android::base::DEBUG || status < 0)
        LOG(status < 0 ? WARNING : DEBUG) << "gpfs op " << op << " " << path << " off " << off
                                          << " size " << size << " -> " << status;
}

}  // namespace angler::fpc
