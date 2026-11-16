/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <aidl/android/hardware/biometrics/fingerprint/ISessionCallback.h>
#include <aidl/android/hardware/keymaster/HardwareAuthToken.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "FpcSensor.h"
#include "FpcTa.h"
#include "NavInput.h"
#include "FsListener.h"
#include "QseeGatekeeper.h"
#include "QseecomLiteChannel.h"

namespace angler::fpc {

/** Cancels one running operation. trigger() is safe from any thread. */
class CancelToken {
  public:
    CancelToken();
    ~CancelToken();
    void trigger();
    bool cancelled() const { return mFlag.load(); }
    int fd() const { return mFd; }
    /** Sleeps up to ms; returns true if cancelled meanwhile. */
    bool sleepMs(int ms);

  private:
    std::atomic<bool> mFlag{false};
    int mFd = -1;
};

/**
 * All the HAL logic: drives the sensor + TA and reports through ISessionCallback. Every method must
 * be called from the single worker thread (see Worker.h); only CancelToken::trigger crosses threads.
 */
class FpcEngine {
  public:
    using Cb = aidl::android::hardware::biometrics::fingerprint::ISessionCallback;
    using Hat = aidl::android::hardware::keymaster::HardwareAuthToken;

    static constexpr int kMaxEnrollments = static_cast<int>(kMaxFingers);

    FpcEngine();
    ~FpcEngine();

    /** Opens sensor and TA. Returns 0 or -errno. */
    int init();

    void selectUser(int userId);

    /** HMAC key for hardware auth tokens (from the shared secret negotiation). */
    void setHatKey(const std::vector<uint8_t>& key);
    uint64_t challenge() const { return mChallenge; }
    int debugStoreDb() { return storeDb(); }

    void generateChallenge(Cb& cb);
    void revokeChallenge(Cb& cb, int64_t challenge);
    void enroll(Cb& cb, const Hat& hat, CancelToken& cancel);
    void authenticate(Cb& cb, int64_t operationId, CancelToken& cancel);
    void detectInteraction(Cb& cb, CancelToken& cancel);
    void enumerateEnrollments(Cb& cb);
    void removeEnrollments(Cb& cb, const std::vector<int32_t>& ids);
    void getAuthenticatorId(Cb& cb);
    void invalidateAuthenticatorId(Cb& cb);
    void resetLockout(Cb& cb, const Hat& hat);

    /** Idle task: navigation gestures until cancelled (stock "default task"). */
    void navigate(CancelToken& cancel);

    // exposed for tests
    static constexpr int kFailsBeforeTimedLockout = 5;
    static constexpr int kFailsBeforePermanentLockout = 20;
    static constexpr int kTimedLockoutMs = 30000;

  private:
    enum class Capture { OK, BAD_IMAGE, CANCELLED, ERROR };

    // BAD_IMAGE: *badInfo says why (too fast / insufficient).
    Capture waitAndCapture(CancelToken& cancel, uint8_t flag,
                           aidl::android::hardware::biometrics::fingerprint::AcquiredInfo* badInfo);
    Capture waitFingerDown(CancelToken& cancel);
    bool reportLockout(Cb& cb);
    void noteFailure(Cb& cb);

    std::string dbPath() const;
    std::string sidPath() const;
    int64_t androidSid();
    int loadDb();
    int storeDb();
    uint64_t authenticatorId();

    FpcSensor mSensor;
    FsListener mFs;
    QseecomLiteChannel mChannel;
    FpcTa mTa;
    QseeGatekeeper mGk;
    NavInput mNav;
    bool mNavOk = false;
    bool mReady = false;
    std::vector<uint8_t> mHatKey;   // shared with software gatekeeper/KeyMint (ISharedSecret)
    uint64_t mChallenge = 0;        // outstanding enrol challenge
    int mUserId = -1;

    int mFailures = 0;
    bool mPermanentLockout = false;
    std::chrono::steady_clock::time_point mLockoutUntil{};
};

}  // namespace angler::fpc
