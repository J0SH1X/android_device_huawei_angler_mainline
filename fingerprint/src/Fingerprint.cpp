/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "fpc-angler"

#include "Fingerprint.h"

#include <android-base/logging.h>

namespace angler::fpc {

Fingerprint::Fingerprint() {
    int rc = mEngine.init();
    mConnected = rc == 0;
    if (!mConnected) LOG(ERROR) << "engine init failed: " << rc;
    else mWorker.setIdleTask([this](CancelToken& c) { mEngine.navigate(c); });
}

ndk::ScopedAStatus Fingerprint::getSensorProps(std::vector<fp::SensorProps>* out) {
    fp::SensorProps p;
    p.commonProps.sensorId = kSensorId;
    p.commonProps.sensorStrength = common::SensorStrength::STRONG;
    p.commonProps.maxEnrollmentsPerUser = FpcEngine::kMaxEnrollments;
    common::ComponentInfo info;
    info.componentId = "fpc1020";
    info.hardwareVersion = "FPC1020";
    info.firmwareVersion = "fingerpr";
    info.serialNumber = "";
    info.softwareVersion = "angler-mainline";
    p.commonProps.componentInfo.push_back(info);
    p.sensorType = fp::FingerprintSensorType::REAR;
    p.supportsDetectInteraction = true;
    p.supportsNavigationGestures = true;
    p.halHandlesDisplayTouches = false;
    p.halControlsIllumination = false;
    out->push_back(std::move(p));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Fingerprint::createSession(int32_t sensorId, int32_t userId,
                                              const std::shared_ptr<fp::ISessionCallback>& cb,
                                              std::shared_ptr<fp::ISession>* out) {
    if (sensorId != kSensorId) {
        return ndk::ScopedAStatus::fromServiceSpecificError(
                static_cast<int32_t>(fp::Error::UNABLE_TO_PROCESS));
    }
    *out = ndk::SharedRefBase::make<Session>(userId, cb, &mEngine, &mWorker);
    return ndk::ScopedAStatus::ok();
}

}  // namespace angler::fpc
