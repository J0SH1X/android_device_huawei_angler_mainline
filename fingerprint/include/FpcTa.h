/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "FpcProtocol.h"
#include "TaChannel.h"

namespace angler::fpc {

using HatBytes = std::array<uint8_t, kHatSize>;

struct IdentifyResult {
    int32_t status = -1;
    uint32_t fid = 0;
    uint32_t coverage = 0;
    uint32_t quality = 0;
    bool matched() const { return fid != 0; }
};

struct EnrolStep {
    int32_t status = -1;
    uint32_t remaining = 0;
};

/**
 * Typed wrapper around the TA command set. Each call is one TrustZone round trip; the SPI clock is
 * held by the hook installed with setClockHook (FpcSensor::ClockGuard in production).
 * Return values: 0 / TA value on success, negative errno for transport failures.
 * Not thread safe; the engine serialises access.
 */
class FpcTa {
  public:
    using ClockHook = std::function<std::function<void()>()>;  // acquire -> returns release

    explicit FpcTa(TaChannel& channel) : mCh(channel) {}
    void setClockHook(ClockHook hook) { mClock = std::move(hook); }

    int init();
    int release();

    // group 1
    int setGid(uint32_t gid);
    int beginEnrol();
    int enrol(EnrolStep* out);
    int endEnrol(uint32_t* fid, int32_t* status);
    int identify(IdentifyResult* out);
    int updateTemplate(int32_t* status);
    // Capture primitives return the raw TA result through *res (see FpcProtocol.h for meaning).
    int fingerLost(int32_t* res);
    int armFingerDown(int32_t* res);
    int readFingerDown(int32_t* res);
    int capture(uint8_t flag, int32_t* res);
    int sensorIdle();
    int getFingerIds(std::vector<uint32_t>* ids);
    int deleteTemplate(uint32_t fid, int32_t* status);
    int dbId(uint64_t* id);

    // group 8: navigation gestures
    int navStart();
    int navStop();
    int navArm();
    int navPoll(uint32_t* gesture, bool* fingerPresent);

    // group 2: db persisted by the TA through the REE FS listener
    int loadEmptyDb();
    int dbLoad(const std::string& path, int32_t* taResult);
    int dbStore(const std::string& path, int32_t* taResult);

    // group 3: hardware auth tokens
    /** Loads the HAT HMAC key into the TA (group 3 cmd 5). */
    int initHwAuth(const std::vector<uint8_t>& key);
    int setAuthChallenge(uint64_t challenge);
    int getEnrolChallenge(uint64_t* challenge);
    int authorizeEnrol(const HatBytes& hat);
    int getAuthResult(HatBytes* hat);

  private:
    // Runs one command. `fill` writes inputs into the buffer, `parse` reads outputs.
    int call(uint32_t group, uint32_t cmd, size_t size,
             const std::function<void(uint8_t*)>& fill,
             const std::function<void(const uint8_t*)>& parse, int32_t* taResult = nullptr);
    int simple(uint32_t cmd, int32_t* result = nullptr) {
        return call(kGroupMain, cmd, kSharedDefault, nullptr, nullptr, result);
    }

    TaChannel& mCh;
    ClockHook mClock;
};

}  // namespace angler::fpc
