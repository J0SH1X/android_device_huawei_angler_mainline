/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "fpc-angler-gk"

#include "QseeGatekeeper.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <fcntl.h>
#include <openssl/rand.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace angler::fpc {

namespace {
constexpr uint32_t kGkEnroll = 0x1001;
constexpr uint32_t kGkVerify = 0x1002;
constexpr size_t kHdr = 0x20;
constexpr size_t kSecretLen = 32;
constexpr size_t kBufTotal = 0xa000;  // stock shared buffer for the keymaster app
// The QSEE gatekeeper slot used for the bridge secret. Android's own gatekeeper is the software
// one, so the QSEE slots are otherwise unused; one slot serves all users (re-enrolled on demand).
constexpr uint32_t kQseeUid = 0;

void put32(std::vector<uint8_t>& b, size_t off, uint32_t v) { memcpy(b.data() + off, &v, 4); }
uint32_t get32(const std::vector<uint8_t>& b, size_t off) {
    uint32_t v;
    memcpy(&v, b.data() + off, 4);
    return v;
}

// Response {status, off, len} -> payload, bounds checked.
int payload(const std::vector<uint8_t>& rsp, std::vector<uint8_t>* out) {
    if (rsp.size() < 12) return -EIO;
    int32_t status = static_cast<int32_t>(get32(rsp, 0));
    uint32_t off = get32(rsp, 4), len = get32(rsp, 8);
    if (status != 0) {
        LOG(ERROR) << "QSEE gatekeeper status " << status;
        return status < 0 ? status : -EIO;
    }
    if (off > rsp.size() || len > rsp.size() - off) return -EIO;
    out->assign(rsp.begin() + off, rsp.begin() + off + len);
    return 0;
}
}  // namespace

std::string QseeGatekeeper::path(int userId) const {
    return mDir + "/gk_" + std::to_string(userId) + ".bin";
}

int QseeGatekeeper::enroll(uint32_t uid, const std::vector<uint8_t>& secret,
                           std::vector<uint8_t>* handle) {
    const size_t dataLen = secret.size();
    std::vector<uint8_t> cmd((kHdr + dataLen + 0x40) & ~size_t(0x3f), 0);
    put32(cmd, 0x00, kGkEnroll);
    put32(cmd, 0x04, uid);
    put32(cmd, 0x18, kHdr);  // new password
    put32(cmd, 0x1c, static_cast<uint32_t>(dataLen));
    memcpy(cmd.data() + kHdr, secret.data(), dataLen);
    std::vector<uint8_t> rsp;
    int rc = mCh.keymasterCall(cmd, (kBufTotal + 0x40 - kHdr - dataLen) & ~size_t(0x3f), &rsp);
    std::fill(cmd.begin(), cmd.end(), 0);
    return rc ? rc : payload(rsp, handle);
}

int QseeGatekeeper::verify(uint32_t uid, uint64_t challenge, const std::vector<uint8_t>& handle,
                           const std::vector<uint8_t>& secret, HatBytes* out) {
    const size_t dataLen = handle.size() + secret.size();
    std::vector<uint8_t> cmd((dataLen + 0x60) & ~size_t(0x3f), 0);
    put32(cmd, 0x00, kGkVerify);
    put32(cmd, 0x04, uid);
    memcpy(cmd.data() + 0x08, &challenge, 8);
    put32(cmd, 0x10, kHdr);
    put32(cmd, 0x14, static_cast<uint32_t>(handle.size()));
    put32(cmd, 0x18, static_cast<uint32_t>(kHdr + handle.size()));
    put32(cmd, 0x1c, static_cast<uint32_t>(secret.size()));
    memcpy(cmd.data() + kHdr, handle.data(), handle.size());
    memcpy(cmd.data() + kHdr + handle.size(), secret.data(), secret.size());
    std::vector<uint8_t> rsp, token;
    int rc = mCh.keymasterCall(cmd, (kBufTotal + 0x20 - dataLen) & ~size_t(0x3f), &rsp);
    std::fill(cmd.begin(), cmd.end(), 0);
    if (rc || (rc = payload(rsp, &token))) return rc;
    if (token.size() != out->size()) {
        LOG(ERROR) << "QSEE gatekeeper token has " << token.size() << " bytes";
        return -EIO;
    }
    memcpy(out->data(), token.data(), out->size());
    return 0;
}

int QseeGatekeeper::loadOrCreate(int userId, bool fresh, std::vector<uint8_t>* secret,
                                 std::vector<uint8_t>* handle) {
    std::string blob;
    if (!fresh && android::base::ReadFileToString(path(userId), &blob) && blob.size() > kSecretLen) {
        secret->assign(blob.begin(), blob.begin() + kSecretLen);
        handle->assign(blob.begin() + kSecretLen, blob.end());
        std::fill(blob.begin(), blob.end(), 0);
        return 0;
    }
    secret->assign(kSecretLen, 0);
    RAND_bytes(secret->data(), kSecretLen);
    if (int rc = enroll(kQseeUid, *secret, handle)) {
        LOG(ERROR) << "QSEE gatekeeper enroll failed: " << rc;
        return rc;
    }
    blob.assign(secret->begin(), secret->end());
    blob.append(handle->begin(), handle->end());
    const std::string tmp = path(userId) + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    bool ok = fd >= 0 && android::base::WriteStringToFd(blob, fd) && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    std::fill(blob.begin(), blob.end(), 0);
    if (!ok || rename(tmp.c_str(), path(userId).c_str()) < 0) {
        PLOG(ERROR) << "store " << path(userId);
        unlink(tmp.c_str());
        return -EIO;
    }
    LOG(INFO) << "QSEE gatekeeper secret enrolled for user " << userId;
    return 0;
}

int QseeGatekeeper::tokenFor(int userId, uint64_t challenge, HatBytes* out) {
    std::vector<uint8_t> secret, handle;
    int rc = loadOrCreate(userId, false, &secret, &handle);
    if (!rc) rc = verify(kQseeUid, challenge, handle, secret, out);
    if (rc) {
        // Stale handle (slot re-enrolled for another user, QSEE state reset): enroll again once.
        LOG(WARNING) << "QSEE gatekeeper verify failed (" << rc << "), re-enrolling";
        rc = loadOrCreate(userId, true, &secret, &handle);
        if (!rc) rc = verify(kQseeUid, challenge, handle, secret, out);
    }
    std::fill(secret.begin(), secret.end(), 0);
    return rc;
}

}  // namespace angler::fpc
