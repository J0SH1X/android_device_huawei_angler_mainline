/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h>
#include <aidl/android/hardware/biometrics/fingerprint/BnSession.h>

#include <memory>

#include "FpcEngine.h"
#include "Worker.h"

namespace angler::fpc {

namespace fp = aidl::android::hardware::biometrics::fingerprint;
namespace common = aidl::android::hardware::biometrics::common;
namespace keymaster = aidl::android::hardware::keymaster;

class Session : public fp::BnSession {
  public:
    Session(int userId, std::shared_ptr<fp::ISessionCallback> cb, FpcEngine* engine, Worker* worker);

    ndk::ScopedAStatus generateChallenge() override;
    ndk::ScopedAStatus revokeChallenge(int64_t challenge) override;
    ndk::ScopedAStatus enroll(const keymaster::HardwareAuthToken& hat,
                              std::shared_ptr<common::ICancellationSignal>* out) override;
    ndk::ScopedAStatus authenticate(int64_t operationId,
                                    std::shared_ptr<common::ICancellationSignal>* out) override;
    ndk::ScopedAStatus detectInteraction(std::shared_ptr<common::ICancellationSignal>* out) override;
    ndk::ScopedAStatus enumerateEnrollments() override;
    ndk::ScopedAStatus removeEnrollments(const std::vector<int32_t>& ids) override;
    ndk::ScopedAStatus getAuthenticatorId() override;
    ndk::ScopedAStatus invalidateAuthenticatorId() override;
    ndk::ScopedAStatus resetLockout(const keymaster::HardwareAuthToken& hat) override;
    ndk::ScopedAStatus close() override;

    // Rear sensor: no display/pointer interaction.
    ndk::ScopedAStatus onPointerDown(int32_t, int32_t, int32_t, float, float) override { return ok(); }
    ndk::ScopedAStatus onPointerUp(int32_t) override { return ok(); }
    ndk::ScopedAStatus onUiReady() override { return ok(); }
    ndk::ScopedAStatus authenticateWithContext(
            int64_t id, const common::OperationContext&,
            std::shared_ptr<common::ICancellationSignal>* out) override {
        return authenticate(id, out);
    }
    ndk::ScopedAStatus enrollWithContext(const keymaster::HardwareAuthToken& hat,
                                         const common::OperationContext&,
                                         std::shared_ptr<common::ICancellationSignal>* out) override {
        return enroll(hat, out);
    }
    ndk::ScopedAStatus detectInteractionWithContext(
            const common::OperationContext&,
            std::shared_ptr<common::ICancellationSignal>* out) override {
        return detectInteraction(out);
    }
    ndk::ScopedAStatus onPointerDownWithContext(const fp::PointerContext&) override { return ok(); }
    ndk::ScopedAStatus onPointerUpWithContext(const fp::PointerContext&) override { return ok(); }
    ndk::ScopedAStatus onContextChanged(const common::OperationContext&) override { return ok(); }
    ndk::ScopedAStatus onPointerCancelWithContext(const fp::PointerContext&) override { return ok(); }
    ndk::ScopedAStatus setIgnoreDisplayTouches(bool) override { return ok(); }

  private:
    static ndk::ScopedAStatus ok() { return ndk::ScopedAStatus::ok(); }
    std::shared_ptr<common::ICancellationSignal> makeSignal(std::shared_ptr<CancelToken> t);

    int mUserId;
    std::shared_ptr<fp::ISessionCallback> mCb;
    FpcEngine* mEngine;
    Worker* mWorker;
};

class Fingerprint : public fp::BnFingerprint {
  public:
    static constexpr int32_t kSensorId = 0;

    /** init() must have succeeded on the engine before the HAL is registered. */
    Fingerprint();
    bool connected() const { return mConnected; }
    void setHatKey(std::vector<uint8_t> key) {
        mWorker.post([this, k = std::move(key)] { mEngine.setHatKey(k); });
    }

    ndk::ScopedAStatus getSensorProps(std::vector<fp::SensorProps>* out) override;
    ndk::ScopedAStatus createSession(int32_t sensorId, int32_t userId,
                                     const std::shared_ptr<fp::ISessionCallback>& cb,
                                     std::shared_ptr<fp::ISession>* out) override;

  private:
    // The worker is declared after the engine so it is destroyed first (it may still use it).
    FpcEngine mEngine;
    bool mConnected = false;
    Worker mWorker;
};

}  // namespace angler::fpc
