/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "fpc-angler"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <aidl/android/hardware/biometrics/fingerprint/BnSessionCallback.h>

#include <cstring>
#include <thread>

#include <unistd.h>

#include "Fingerprint.h"
#include "Hat.h"
#include "SharedSecret.h"

using angler::fpc::CancelToken;
using angler::fpc::Fingerprint;
using angler::fpc::FpcEngine;
namespace fp = aidl::android::hardware::biometrics::fingerprint;

namespace {

// Logs every callback; used by --selftest to drive the engine without the framework.
class LogCallback : public fp::BnSessionCallback {
  public:
    using A = ndk::ScopedAStatus;
    A onChallengeGenerated(int64_t c) override { return log("challenge generated " + std::to_string(c)); }
    A onChallengeRevoked(int64_t c) override { return log("challenge revoked " + std::to_string(c)); }
    A onAcquired(fp::AcquiredInfo i, int32_t v) override {
        return log("acquired " + fp::toString(i) + " " + std::to_string(v));
    }
    A onError(fp::Error e, int32_t v) override { return log("ERROR " + fp::toString(e) + " " + std::to_string(v)); }
    A onEnrollmentProgress(int32_t id, int32_t r) override {
        return log("enroll progress id " + std::to_string(id) + " remaining " + std::to_string(r));
    }
    A onAuthenticationSucceeded(int32_t id, const aidl::android::hardware::keymaster::HardwareAuthToken&) override {
        return log("AUTH OK fid " + std::to_string(id));
    }
    A onAuthenticationFailed() override { return log("auth failed (no match)"); }
    A onLockoutTimed(int64_t ms) override { return log("lockout " + std::to_string(ms)); }
    A onLockoutPermanent() override { return log("lockout permanent"); }
    A onLockoutCleared() override { return log("lockout cleared"); }
    A onInteractionDetected() override { return log("FINGER DETECTED"); }
    A onEnrollmentsEnumerated(const std::vector<int32_t>& ids) override {
        std::string s = "enrollments:";
        for (int32_t i : ids) s += " " + std::to_string(i);
        return log(s);
    }
    A onEnrollmentsRemoved(const std::vector<int32_t>&) override { return log("removed"); }
    A onAuthenticatorIdRetrieved(int64_t id) override { return log("authenticator id " + std::to_string(id)); }
    A onAuthenticatorIdInvalidated(int64_t id) override { return log("authenticator id now " + std::to_string(id)); }
    A onSessionClosed() override { return log("session closed"); }
    A onAuthenticationSucceededWithResult(const fp::AuthenticateSuccess& r) override {
        return log("AUTH OK fid " + std::to_string(r.enrollmentId));
    }

  private:
    A log(const std::string& s) {
        LOG(INFO) << "selftest: " << s;
        printf("selftest: %s\n", s.c_str());
        fflush(stdout);
        return A::ok();
    }
};

// Bring-up checks without servicemanager/VINTF. Uses gid 99 so real users are untouched.
//   --selftest          TA queries + wait up to 15 s for a finger
//   --selftest enroll   full enrollment (HAT signed with a test key), 90 s
//   --selftest auth     one authentication attempt, 30 s
int selftest(const char* mode) {
    constexpr int kTestUser = 99;
    FpcEngine engine;
    int rc = engine.init();
    printf("selftest: init %d\n", rc);
    if (rc) return EXIT_FAILURE;
    auto cb = ndk::SharedRefBase::make<LogCallback>();
    engine.selectUser(kTestUser);
    engine.enumerateEnrollments(*cb);
    engine.getAuthenticatorId(*cb);

    const std::vector<uint8_t> testKey(32, 0x5a);
    engine.setHatKey(testKey);
    int seconds = 15;
    if (mode && strcmp(mode, "enroll") == 0) seconds = 90;
    if (mode && strcmp(mode, "auth") == 0) seconds = 30;

    CancelToken cancel;
    std::thread t([&] {
        for (int i = 0; i < seconds * 10 && !cancel.cancelled(); i++) usleep(100000);
        cancel.trigger();
    });
    if (mode && strcmp(mode, "store") == 0) {
        android::base::SetMinimumLogSeverity(android::base::DEBUG);
        printf("selftest: store db -> %d\n", engine.debugStoreDb());
    } else if (mode && strcmp(mode, "enroll") == 0) {
        engine.generateChallenge(*cb);
        aidl::android::hardware::keymaster::HardwareAuthToken hat;
        hat.challenge = static_cast<int64_t>(engine.challenge());
        hat.userId = 1;
        hat.authenticatorId = 0;
        hat.authenticatorType = aidl::android::hardware::keymaster::HardwareAuthenticatorType::PASSWORD;
        hat.timestamp.milliSeconds = 1;
        hat.mac = angler::fpc::hatMac(hat, testKey);
        printf("selftest: enrolling, lift and touch repeatedly (%d s)\n", seconds);
        fflush(stdout);
        engine.enroll(*cb, hat, cancel);
    } else if (mode && strcmp(mode, "auth") == 0) {
        printf("selftest: authenticating, touch the sensor (%d s)\n", seconds);
        fflush(stdout);
        engine.authenticate(*cb, 12345, cancel);
    } else if (!mode) {
        printf("selftest: touch the sensor within %d s\n", seconds);
        fflush(stdout);
        engine.detectInteraction(*cb, cancel);
    }
    cancel.trigger();
    t.join();
    engine.enumerateEnrollments(*cb);
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    android::base::InitLogging(argv, android::base::LogdLogger(android::base::SYSTEM));
    android::base::SetDefaultTag(LOG_TAG);
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) return selftest(argc > 2 ? argv[2] : nullptr);
    ABinderProcess_setThreadPoolMaxThreadCount(1);

    auto hal = ndk::SharedRefBase::make<Fingerprint>();
    if (!hal->connected()) {
        // oneshot service: do not crash-loop while the kernel driver is missing
        LOG(ERROR) << "fingerprint hardware not available, not registering";
        return EXIT_FAILURE;
    }

    // Join keystore2's HMAC key agreement first: it hands us the HAT key for the TA.
    auto ss = ndk::SharedRefBase::make<angler::fpc::SharedSecret>(
            [hal](std::vector<uint8_t> key) { hal->setHatKey(std::move(key)); });
    const std::string ssInstance = std::string(angler::fpc::SharedSecret::descriptor) + "/fingerprint";
    binder_status_t status = AServiceManager_addService(ss->asBinder().get(), ssInstance.c_str());
    CHECK_EQ(status, STATUS_OK);

    const std::string instance = std::string(Fingerprint::descriptor) + "/default";
    status = AServiceManager_addService(hal->asBinder().get(), instance.c_str());
    CHECK_EQ(status, STATUS_OK);
    LOG(INFO) << "registered " << instance << " and " << ssInstance;

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // not reached
}
