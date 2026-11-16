/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "fpc-angler-ta"

#include "FpcTa.h"

#include <android-base/logging.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace angler::fpc {

namespace {
inline void put32(uint8_t* b, size_t off, uint32_t v) { memcpy(b + off, &v, 4); }
inline void put64(uint8_t* b, size_t off, uint64_t v) { memcpy(b + off, &v, 8); }
inline uint32_t get32(const uint8_t* b, size_t off) {
    uint32_t v;
    memcpy(&v, b + off, 4);
    return v;
}
inline int32_t geti32(const uint8_t* b, size_t off) { return static_cast<int32_t>(get32(b, off)); }
inline uint64_t get64(const uint8_t* b, size_t off) {
    uint64_t v;
    memcpy(&v, b + off, 8);
    return v;
}
}  // namespace

int FpcTa::call(uint32_t group, uint32_t cmd, size_t size,
                const std::function<void(uint8_t*)>& fill,
                const std::function<void(const uint8_t*)>& parse, int32_t* taResult) {
    std::vector<uint8_t> buf(size, 0);
    put32(buf.data(), kOffGroup, group);
    put32(buf.data(), kOffCmd, cmd);
    if (fill) fill(buf.data());

    std::function<void()> release;
    if (mClock) release = mClock();
    int rc = mCh.transact(buf.data(), buf.size());
    if (release) release();
    if (rc != 0) {
        LOG(ERROR) << "TA transact group " << group << " cmd 0x" << std::hex << cmd << std::dec
                   << " failed: " << rc;
        return rc;
    }
    if (parse) parse(buf.data());
    if (taResult) *taResult = geti32(buf.data(), kOffResult);
    return 0;
}

int FpcTa::init() { return simple(kCmdSensorIdle); }
int FpcTa::release() { return simple(kCmdSensorIdle); }
int FpcTa::sensorIdle() { return simple(kCmdSensorIdle); }
int FpcTa::capture(uint8_t flag, int32_t* res) {
    return call(kGroupMain, kCmdCapture, kSharedDefault,
                [&](uint8_t* b) { b[kOffOut] = flag; }, nullptr, res);
}

int FpcTa::setGid(uint32_t gid) {
    int32_t res = -1;
    int rc = call(kGroupMain, kCmdSetGid, kSharedDefault,
                  [&](uint8_t* b) { put32(b, kOffResult, gid); },
                  [&](const uint8_t* b) { res = geti32(b, kOffOut); });
    return rc ? rc : res;
}

int FpcTa::beginEnrol() {
    int32_t res = -1;
    int rc = call(kGroupMain, kCmdBeginEnrol, kSharedDefault, nullptr, nullptr, &res);
    return rc ? rc : res;
}

int FpcTa::enrol(EnrolStep* out) {
    return call(kGroupMain, kCmdEnrol, kSharedDefault, nullptr, [&](const uint8_t* b) {
        out->status = geti32(b, kOffResult);
        out->remaining = get32(b, kOffOut);
    });
}

int FpcTa::endEnrol(uint32_t* fid, int32_t* status) {
    return call(kGroupMain, kCmdEndEnrol, kSharedDefault, nullptr, [&](const uint8_t* b) {
        *fid = static_cast<uint32_t>(geti32(b, kOffResult));
        *status = geti32(b, kOffOut);
    });
}

int FpcTa::identify(IdentifyResult* out) {
    return call(kGroupMain, kCmdIdentify, kSharedDefault, nullptr, [&](const uint8_t* b) {
        out->status = geti32(b, kOffResult);
        out->fid = get32(b, kOffOut);
        out->coverage = get32(b, kOffPayload);
        out->quality = get32(b, kOffPayload + 4);
    });
}

int FpcTa::updateTemplate(int32_t* status) {
    return call(kGroupMain, kCmdUpdateTemplate, kSharedDefault, nullptr, nullptr, status);
}

int FpcTa::fingerLost(int32_t* res) {
    return call(kGroupMain, kCmdFingerLost, kSharedDefault, nullptr, nullptr, res);
}

int FpcTa::armFingerDown(int32_t* res) {
    return call(kGroupMain, kCmdArmFingerDown, kSharedDefault, nullptr, nullptr, res);
}

int FpcTa::readFingerDown(int32_t* res) {
    return call(kGroupMain, kCmdReadFingerDown, kSharedDefault, nullptr, nullptr, res);
}

int FpcTa::getFingerIds(std::vector<uint32_t>* ids) {
    // Stock (fpc_tee_get_finger_ids): the common 0x48 shared buffer, count in/out at +0xc
    // (must be < 6), ids at +0x10, TA status at +8.
    uint32_t count = 0;
    int32_t status = -1;
    std::vector<uint32_t> out;
    int rc = call(kGroupMain, kCmdGetFingerIds, kSharedDefault,
                  [&](uint8_t* b) { put32(b, kOffOut, kMaxFingers); },
                  [&](const uint8_t* b) {
                      status = geti32(b, kOffResult);
                      count = std::min<uint32_t>(get32(b, kOffOut), kMaxFingers);
                      for (uint32_t i = 0; i < count; i++) out.push_back(get32(b, kOffPayload + 4 * i));
                  });
    if (rc) return rc;
    if (status != kTaOk) return status;
    *ids = std::move(out);
    return 0;
}

int FpcTa::deleteTemplate(uint32_t fid, int32_t* status) {
    return call(kGroupMain, kCmdDeleteTemplate, kSharedDefault,
                [&](uint8_t* b) { put32(b, kOffResult, fid); },
                [&](const uint8_t* b) { *status = geti32(b, kOffOut); });
}

int FpcTa::dbId(uint64_t* id) {
    int32_t status = -1;
    int rc = call(kGroupMain, kCmdDbId, kSharedDefault, nullptr, [&](const uint8_t* b) {
        *id = get64(b, kOffResult);
        status = geti32(b, kOffPayload);
    });
    return rc ? rc : status;
}

// ---- group 8 ---------------------------------------------------------------------------------------

int FpcTa::navStart() {
    int32_t res = -1;
    int rc = call(kGroupNav, kCmdNavStart, kSharedDefault, nullptr, nullptr, &res);
    return rc ? rc : res;
}

int FpcTa::navStop() {
    int32_t res = -1;
    int rc = call(kGroupNav, kCmdNavStop, kSharedDefault, nullptr, nullptr, &res);
    return rc ? rc : res;
}

int FpcTa::navArm() {
    int32_t res = -1;
    int rc = call(kGroupNav, kCmdNavArm, kSharedDefault, [](uint8_t* b) { put32(b, kOffOut, 0); },
                  nullptr, &res);
    return rc ? rc : res;
}

int FpcTa::navPoll(uint32_t* gesture, bool* fingerPresent) {
    int32_t res = -1;
    int rc = call(kGroupNav, kCmdNavPoll, kSharedDefault, nullptr,
                  [&](const uint8_t* b) {
                      uint32_t g = get32(b, kOffOut);
                      *gesture = (g >= 1 && g <= 6) ? g : 0;
                      *fingerPresent = b[kOffPayload] != 0;
                  },
                  &res);
    return rc ? rc : res;
}

// ---- group 2 ---------------------------------------------------------------------------------------

int FpcTa::loadEmptyDb() {
    int32_t res = -1;
    int rc = call(kGroupMain, kCmdLoadEmptyDb, kSharedDefault, nullptr, nullptr, &res);
    return rc ? rc : res;
}

int FpcTa::dbLoad(const std::string& path, int32_t* taResult) {
    const size_t size = path.size() + 0x11;
    return call(kGroupDb, kCmdDbLoad, size,
                [&](uint8_t* b) {
                    put32(b, kOffOut, static_cast<uint32_t>(path.size() + 1));
                    memcpy(b + kOffPayload, path.c_str(), path.size() + 1);
                },
                nullptr, taResult);
}

int FpcTa::dbStore(const std::string& path, int32_t* taResult) {
    const size_t size = path.size() + 0x11;
    return call(kGroupDb, kCmdDbStore, size,
                [&](uint8_t* b) {
                    put32(b, kOffOut, static_cast<uint32_t>(path.size() + 1));
                    memcpy(b + kOffPayload, path.c_str(), path.size() + 1);
                },
                nullptr, taResult);
}

// ---- group 3 ---------------------------------------------------------------------------------------

int FpcTa::setAuthChallenge(uint64_t challenge) {
    int32_t status = -1;
    int rc = call(kGroupAuth, kCmdSetAuthChallenge, kSharedDefault,
                  [&](uint8_t* b) { put64(b, kOffResult, challenge); },
                  [&](const uint8_t* b) { status = geti32(b, kOffPayload); });
    return rc ? rc : status;
}

int FpcTa::getEnrolChallenge(uint64_t* challenge) {
    int32_t status = -1;
    int rc = call(kGroupAuth, kCmdGetEnrolChallenge, kSharedDefault, nullptr,
                  [&](const uint8_t* b) {
                      *challenge = get64(b, kOffResult);
                      status = geti32(b, kOffPayload);
                  });
    return rc ? rc : status;
}

int FpcTa::authorizeEnrol(const HatBytes& hat) {
    int32_t res = -1;
    int rc = call(kGroupAuth, kCmdAuthorizeEnrol, kSharedAuthToken,
                  [&](uint8_t* b) {
                      put32(b, kOffOut, kHatSize);
                      memcpy(b + kOffPayload, hat.data(), kHatSize);
                  },
                  nullptr, &res);
    return rc ? rc : res;
}

int FpcTa::getAuthResult(HatBytes* hat) {
    int32_t res = -1;
    int rc = call(kGroupAuth, kCmdGetAuthResult, kSharedAuthToken,
                  [&](uint8_t* b) { put32(b, kOffOut, kHatSize); },
                  [&](const uint8_t* b) { memcpy(hat->data(), b + kOffPayload, kHatSize); }, &res);
    return rc ? rc : res;
}

int FpcTa::initHwAuth(const std::vector<uint8_t>& key) {
    if (key.empty() || key.size() > kHatKeyMax) return -EINVAL;
    int32_t res = -1;
    int rc = call(kGroupAuth, kCmdInitHwAuth, kOffPayload + key.size(),
                  [&](uint8_t* b) {
                      put32(b, kOffOut, static_cast<uint32_t>(key.size()));
                      memcpy(b + kOffPayload, key.data(), key.size());
                  },
                  nullptr, &res);
    return rc ? rc : res;
}

}  // namespace angler::fpc
