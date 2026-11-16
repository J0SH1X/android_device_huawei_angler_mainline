/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "fpc-angler-engine"

#include "FpcEngine.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <thread>
#include <cerrno>

#include "Hat.h"

namespace angler::fpc {

using aidl::android::hardware::biometrics::fingerprint::AcquiredInfo;
using aidl::android::hardware::biometrics::fingerprint::Error;

namespace {
constexpr const char* kDataRoot = "/data/vendor/fpc";
constexpr int kFingerLostPollMs = 20;

}  // namespace

// ---- CancelToken -------------------------------------------------------------------------------

CancelToken::CancelToken() : mFd(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}
CancelToken::~CancelToken() {
    if (mFd >= 0) close(mFd);
}
void CancelToken::trigger() {
    mFlag = true;
    uint64_t one = 1;
    if (mFd >= 0) (void)!write(mFd, &one, sizeof(one));
}
bool CancelToken::sleepMs(int ms) {
    pollfd p = {mFd, POLLIN, 0};
    poll(&p, 1, ms);
    return cancelled();
}

// ---- lifecycle ---------------------------------------------------------------------------------

FpcEngine::FpcEngine()
    : mFs(kDataRoot, {"/odm/firmware", "/vendor/firmware"}),
      mChannel(mFs),
      mTa(mChannel),
      mGk(mChannel, kDataRoot) {}

FpcEngine::~FpcEngine() {
    if (mReady) {
        mTa.release();
        mChannel.close();
    }
}

int FpcEngine::init() {
    mkdir("/data/vendor", 0771);  // normally created by init.rc; harmless if it exists
    mkdir(kDataRoot, 0700);

    if (int rc = mSensor.open()) return rc;
    mTa.setClockHook([this]() -> std::function<void()> {
        mSensor.setClock(true);
        return [this] { mSensor.setClock(false); };
    });

    // The TA load itself needs the SPI clock (stock: clk_enable around QSEECom_start_app).
    mSensor.setClock(true);
    int rc = mChannel.open();
    mSensor.setClock(false);
    if (rc) return rc;

    if ((rc = mTa.init())) return rc;

    // The TA only accepts its own (TrustZone-encapsulated) key from the QSEE keymaster, exactly as
    // stock does. Tokens between the TA and the (software) gatekeeper/KeyMint are bridged with the
    // shared-secret key in setHatKey(), see enroll()/authenticate().
    std::vector<uint8_t> qseeKey;
    if ((rc = mChannel.getAuthTokenKey(&qseeKey)) == 0) rc = mTa.initHwAuth(qseeKey);
    std::fill(qseeKey.begin(), qseeKey.end(), 0);
    if (rc) LOG(WARNING) << "init_hw_auth with the QSEE keymaster key failed: " << rc;

    mNavOk = mNav.open() == 0;
    mReady = true;
    return 0;
}

void FpcEngine::setHatKey(const std::vector<uint8_t>& key) {
    mHatKey = key;
    LOG(INFO) << "shared HAT key set";
}

std::string FpcEngine::sidPath() const {
    return std::string(kDataRoot) + "/sid_" + std::to_string(mUserId);
}

// Android's secure user id for the current user, recorded from the last verified enrol HAT.
int64_t FpcEngine::androidSid() {
    std::string s;
    if (!android::base::ReadFileToString(sidPath(), &s)) return 0;
    return strtoll(s.c_str(), nullptr, 10);
}

std::string FpcEngine::dbPath() const {
    return std::string(kDataRoot) + "/user_" + std::to_string(mUserId) + ".db";
}

int FpcEngine::loadDb() {
    const std::string path = dbPath();
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && st.st_size > 0) {
        int32_t res = -1;
        int rc = mTa.dbLoad(path, &res);
        if (rc == 0 && res == kTaOk) return 0;
        LOG(WARNING) << "template db load failed (rc " << rc << ", ta " << res
                     << "), starting empty" << (res == kTaDbCorrupt ? " [corrupt]" : "");
    }
    return mTa.loadEmptyDb();
}

int FpcEngine::storeDb() {
    const std::string path = dbPath();
    const std::string tmp = path + ".tmp";
    int32_t res = -1;
    int rc = mTa.dbStore(tmp, &res);
    if (rc) return rc;
    if (res != kTaOk) {
        LOG(ERROR) << "template db store failed: " << res;
        unlink(tmp.c_str());
        return -EIO;
    }
    if (rename(tmp.c_str(), path.c_str()) < 0) {
        PLOG(ERROR) << "rename " << tmp;
        return -errno;
    }
    return 0;
}

void FpcEngine::selectUser(int userId) {
    if (!mReady || userId == mUserId) return;
    if (mUserId >= 0) storeDb();  // persist whatever the previous user changed
    mUserId = userId;
    int rc = mTa.setGid(static_cast<uint32_t>(userId));
    if (rc) LOG(ERROR) << "set_gid " << userId << " failed: " << rc;
    rc = loadDb();
    if (rc) LOG(ERROR) << "template db load for user " << userId << " failed: " << rc;
}

uint64_t FpcEngine::authenticatorId() {
    std::vector<uint32_t> ids;
    if (mTa.getFingerIds(&ids) || ids.empty()) return 0;
    uint64_t id = 0;
    return mTa.dbId(&id) == 0 ? id : 0;
}

// ---- finger detection + capture ----------------------------------------------------------------

FpcEngine::Capture FpcEngine::waitFingerDown(CancelToken& cancel) {
    // The previous finger must have left first, or a resting finger would be matched repeatedly.
    for (;;) {
        int32_t res = 0;
        if (mTa.fingerLost(&res) || res < 0) {
            LOG(ERROR) << "finger_lost: " << res;
            return Capture::ERROR;
        }
        if (res > 0) break;
        LOG(VERBOSE) << "finger still on sensor";
        if (cancel.sleepMs(kFingerLostPollMs)) {
            LOG(INFO) << "cancelled while waiting for finger lift";
            return Capture::CANCELLED;
        }
    }
    for (;;) {
        if (cancel.cancelled()) return Capture::CANCELLED;
        int32_t res = 0;
        if (mTa.armFingerDown(&res) || res != 0) {
            LOG(ERROR) << "arm_finger_down: " << res;
            return Capture::ERROR;
        }
        LOG(INFO) << "finger detect armed, waiting for irq";
        switch (mSensor.waitIrq(cancel.fd(), -1)) {
            case FpcSensor::Wait::CANCELLED:
                LOG(INFO) << "cancelled while waiting for irq";
                return Capture::CANCELLED;
            case FpcSensor::Wait::IRQ: break;
            default: return Capture::ERROR;
        }
        mSensor.ackIrq();
        if (mTa.readFingerDown(&res) || res < 0) {
            LOG(ERROR) << "read_finger_down: " << res;
            return Capture::ERROR;
        }
        LOG(INFO) << "irq, read_finger_down " << res;
        if (res > 0) return Capture::OK;
        // 0: spurious interrupt, re-arm
    }
}

FpcEngine::Capture FpcEngine::waitAndCapture(CancelToken& cancel, uint8_t flag,
                                              AcquiredInfo* badInfo) {
    mSensor.setWakeup(true);
    Capture r = waitFingerDown(cancel);
    if (r == Capture::OK) {
        int32_t res = 0;
        if (mTa.capture(flag, &res)) {
            r = Capture::ERROR;
        } else if (res == kCaptureTooFast || res == kCaptureInsufficient) {
            *badInfo = res == kCaptureTooFast ? AcquiredInfo::TOO_FAST : AcquiredInfo::INSUFFICIENT;
            r = Capture::BAD_IMAGE;
        } else if (res == kTaErrCancelled) {
            r = Capture::CANCELLED;
        } else if (res != kCaptureOk) {
            LOG(ERROR) << "capture: " << res;
            r = Capture::ERROR;
        }
    }
    mSensor.setWakeup(false);
    mTa.sensorIdle();
    return r;
}

// ---- challenge ---------------------------------------------------------------------------------

void FpcEngine::generateChallenge(Cb& cb) {
    uint64_t c = 0;
    if (!mReady || mTa.getEnrolChallenge(&c)) {
        cb.onError(Error::HW_UNAVAILABLE, 0);
        return;
    }
    mChallenge = c;
    cb.onChallengeGenerated(static_cast<int64_t>(c));
}

void FpcEngine::revokeChallenge(Cb& cb, int64_t challenge) {
    if (mReady) mTa.setAuthChallenge(0);
    if (static_cast<uint64_t>(challenge) == mChallenge) mChallenge = 0;
    cb.onChallengeRevoked(challenge);
}

// ---- enroll ------------------------------------------------------------------------------------

void FpcEngine::enroll(Cb& cb, const Hat& hat, CancelToken& cancel) {
    if (!mReady) { cb.onError(Error::HW_UNAVAILABLE, 0); return; }
    if (cancel.cancelled()) { cb.onError(Error::CANCELED, 0); return; }

    std::vector<uint32_t> ids;
    if (mTa.getFingerIds(&ids) == 0 && ids.size() >= static_cast<size_t>(kMaxEnrollments)) {
        cb.onError(Error::NO_SPACE, 0);
        return;
    }

    // The enrol HAT comes from the software gatekeeper: verify it with the shared key and the
    // outstanding challenge here, since the TA can only verify tokens signed with its QSEE key.
    if (mHatKey.empty() || !hatVerify(hat, mHatKey)) {
        LOG(ERROR) << "enroll: HAT does not verify with the shared key";
        cb.onError(Error::UNABLE_TO_PROCESS, 0);
        return;
    }
    if (mChallenge == 0 || static_cast<uint64_t>(hat.challenge) != mChallenge) {
        LOG(ERROR) << "enroll: HAT challenge does not match";
        cb.onError(Error::UNABLE_TO_PROCESS, 0);
        return;
    }
    HatBytes bytes;
    if (!hatToBytes(hat, &bytes)) {
        cb.onError(Error::UNABLE_TO_PROCESS, 0);
        return;
    }
    // Trade the (verified) software token for a QSEE-signed one with the same challenge.
    if (int rc = mGk.tokenFor(mUserId, mChallenge, &bytes)) {
        LOG(ERROR) << "enroll: no QSEE token: " << rc;
        cb.onError(Error::UNABLE_TO_PROCESS, 0);
        return;
    }
    if (int rc = mTa.authorizeEnrol(bytes)) {
        LOG(ERROR) << "authorize_enrol: " << rc;
        cb.onError(Error::UNABLE_TO_PROCESS, 0);
        return;
    }
    android::base::WriteStringToFile(std::to_string(hat.userId), sidPath(), 0600, geteuid(),
                                     getegid());
    if (int rc = mTa.beginEnrol()) {
        LOG(ERROR) << "begin_enrol: " << rc;
        cb.onError(Error::UNABLE_TO_PROCESS, 0);
        return;
    }

    // Mirrors the stock do_enroll status handling.
    for (;;) {
        AcquiredInfo bad = AcquiredInfo::UNKNOWN;
        switch (waitAndCapture(cancel, kCaptureFlagEnrol, &bad)) {
            case Capture::CANCELLED: cb.onError(Error::CANCELED, 0); return;
            case Capture::ERROR: cb.onError(Error::HW_UNAVAILABLE, 0); return;
            case Capture::BAD_IMAGE: cb.onAcquired(bad, 0); continue;
            case Capture::OK: break;
        }
        EnrolStep step;
        if (mTa.enrol(&step) || step.status < 0) {
            int32_t st = step.status;
            cb.onError(st == kTaErrNoSpace ? Error::NO_SPACE
                       : st == kTaErrTimeout ? Error::TIMEOUT
                                             : Error::HW_UNAVAILABLE,
                       0);
            return;
        }
        const int32_t remaining = static_cast<int32_t>(step.remaining);
        LOG(INFO) << "enrol step: status " << step.status << " remaining " << remaining;
        switch (step.status) {
            case kEnrolDone: {
                uint32_t fid = 0;
                int32_t status = -1;
                int rc = mTa.endEnrol(&fid, &status);
                LOG(INFO) << "end_enrol: rc " << rc << " status " << status << " fid " << fid;
                if (rc || status != kTaOk) {
                    cb.onError(Error::UNABLE_TO_PROCESS, 0);
                    return;
                }
                if (storeDb()) {
                    loadDb();  // drop the template that could not be persisted, like stock
                    cb.onError(Error::UNABLE_TO_PROCESS, 0);
                    return;
                }
                cb.onEnrollmentProgress(static_cast<int32_t>(fid), 0);
                return;
            }
            case kEnrolAbort:
                LOG(ERROR) << "TA aborted the enrollment";
                cb.onError(Error::UNABLE_TO_PROCESS, 0);
                return;
            case kEnrolAbortVendor: cb.onError(Error::VENDOR, 0); return;
            case kEnrolGood: cb.onAcquired(AcquiredInfo::GOOD, 0); break;
            case kEnrolPartial: cb.onAcquired(AcquiredInfo::PARTIAL, 0); break;
            case kEnrolVendor: cb.onAcquired(AcquiredInfo::VENDOR, 0); break;
            case kEnrolDirty: cb.onAcquired(AcquiredInfo::SENSOR_DIRTY, 0); break;
            default: break;
        }
        cb.onEnrollmentProgress(0, remaining);
    }
}

// ---- lockout -----------------------------------------------------------------------------------

bool FpcEngine::reportLockout(Cb& cb) {
    if (mPermanentLockout) {
        cb.onLockoutPermanent();
        return true;
    }
    auto now = std::chrono::steady_clock::now();
    if (now < mLockoutUntil) {
        cb.onLockoutTimed(
                std::chrono::duration_cast<std::chrono::milliseconds>(mLockoutUntil - now).count());
        return true;
    }
    return false;
}

void FpcEngine::noteFailure(Cb& cb) {
    mFailures++;
    if (mFailures >= kFailsBeforePermanentLockout) {
        mPermanentLockout = true;
        cb.onLockoutPermanent();
    } else if (mFailures % kFailsBeforeTimedLockout == 0) {
        mLockoutUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimedLockoutMs);
        cb.onLockoutTimed(kTimedLockoutMs);
    }
}

void FpcEngine::resetLockout(Cb& cb, const Hat& hat) {
    HatBytes bytes;
    // The HAT must be well formed; TA-side verification of the HMAC happens in authorize_enrol, which
    // also consumes a challenge, so a resetLockout token is only checked for shape here.
    if (!hatToBytes(hat, &bytes)) {
        cb.onError(Error::UNABLE_TO_PROCESS, 0);
        return;
    }
    mFailures = 0;
    mPermanentLockout = false;
    mLockoutUntil = {};
    cb.onLockoutCleared();
}

// ---- authenticate ------------------------------------------------------------------------------

void FpcEngine::authenticate(Cb& cb, int64_t operationId, CancelToken& cancel) {
    if (!mReady) { cb.onError(Error::HW_UNAVAILABLE, 0); return; }
    if (cancel.cancelled()) { cb.onError(Error::CANCELED, 0); return; }
    if (reportLockout(cb)) return;

    if (mTa.setAuthChallenge(static_cast<uint64_t>(operationId)) != 0) {
        cb.onError(Error::UNABLE_TO_PROCESS, 0);
        return;
    }
    // Mirrors the stock do_authenticate.
    for (;;) {
        AcquiredInfo bad = AcquiredInfo::UNKNOWN;
        switch (waitAndCapture(cancel, kCaptureFlagAuth, &bad)) {
            case Capture::CANCELLED: cb.onError(Error::CANCELED, 0); return;
            case Capture::ERROR: cb.onError(Error::HW_UNAVAILABLE, 0); return;
            case Capture::BAD_IMAGE: cb.onAcquired(bad, 0); continue;
            case Capture::OK: break;
        }
        cb.onAcquired(AcquiredInfo::GOOD, 0);
        IdentifyResult id;
        if (mTa.identify(&id) || id.status != kTaOk) {
            LOG(ERROR) << "identify: " << id.status;
            cb.onError(Error::HW_UNAVAILABLE, 0);
            return;
        }
        LOG(INFO) << "identify: fid " << id.fid << " coverage " << id.coverage << " quality "
                  << id.quality;

        if (!id.matched()) {
            cb.onAuthenticationFailed();
            noteFailure(cb);
            if (mPermanentLockout || std::chrono::steady_clock::now() < mLockoutUntil) return;
            continue;  // lifecycle does not end on a rejected finger
        }

        HatBytes bytes;
        Hat hat;
        if (mTa.getAuthResult(&bytes) != 0 || !hatFromBytes(bytes, &hat)) {
            cb.onError(Error::UNABLE_TO_PROCESS, 0);
            return;
        }
        // The TA signed with its QSEE key; keystore verifies with the shared key. Re-sign.
        if (!mHatKey.empty()) {
            // The TA token carries the QSEE gatekeeper user; keystore expects Android's SID.
            if (int64_t sid = androidSid()) hat.userId = sid;
            std::vector<uint8_t> mac = hatMac(hat, mHatKey);
            if (!mac.empty()) hat.mac = std::move(mac);
        } else {
            LOG(WARNING) << "no shared HAT key yet; keystore will reject this token";
        }
        mFailures = 0;
        cb.onAuthenticationSucceeded(static_cast<int32_t>(id.fid), hat);

        // Adaptive template update; 0 = changed, persist (stock behaviour).
        int32_t upd = -1;
        if (mTa.updateTemplate(&upd) == 0 && upd == kTaOk) storeDb();
        return;
    }
}

void FpcEngine::detectInteraction(Cb& cb, CancelToken& cancel) {
    if (!mReady) { cb.onError(Error::HW_UNAVAILABLE, 0); return; }
    if (cancel.cancelled()) { cb.onError(Error::CANCELED, 0); return; }
    AcquiredInfo bad = AcquiredInfo::UNKNOWN;
    switch (waitAndCapture(cancel, kCaptureFlagAuth, &bad)) {
        case Capture::CANCELLED: cb.onError(Error::CANCELED, 0); return;
        case Capture::ERROR: cb.onError(Error::HW_UNAVAILABLE, 0); return;
        case Capture::BAD_IMAGE:  // a finger was down; that is all detectInteraction needs
        case Capture::OK: cb.onInteractionDetected(); return;
    }
}

// ---- enrollments -------------------------------------------------------------------------------

void FpcEngine::enumerateEnrollments(Cb& cb) {
    std::vector<uint32_t> ids;
    if (!mReady || mTa.getFingerIds(&ids)) {
        cb.onError(Error::HW_UNAVAILABLE, 0);
        return;
    }
    cb.onEnrollmentsEnumerated(std::vector<int32_t>(ids.begin(), ids.end()));
}

void FpcEngine::removeEnrollments(Cb& cb, const std::vector<int32_t>& ids) {
    if (!mReady) { cb.onError(Error::HW_UNAVAILABLE, 0); return; }
    std::vector<int32_t> removed;
    bool failed = false;
    for (int32_t fid : ids) {
        int32_t st = -1;
        if (mTa.deleteTemplate(static_cast<uint32_t>(fid), &st) == 0 && st == kTaOk) {
            removed.push_back(fid);
        } else {
            failed = true;
        }
    }
    if (!removed.empty() && storeDb()) failed = true;
    if (failed) {
        cb.onError(Error::UNABLE_TO_REMOVE, 0);
        return;
    }
    cb.onEnrollmentsRemoved(removed);
}

void FpcEngine::getAuthenticatorId(Cb& cb) {
    cb.onAuthenticatorIdRetrieved(mReady ? static_cast<int64_t>(authenticatorId()) : 0);
}

void FpcEngine::invalidateAuthenticatorId(Cb& cb) {
    // No TA command regenerates the id explicitly; the TA derives it from the template set, so the
    // current value is the best answer we can give.
    cb.onAuthenticatorIdInvalidated(mReady ? static_cast<int64_t>(authenticatorId()) : 0);
}

// ---- navigation ---------------------------------------------------------------------------------

// Port of the stock fpc_tee_nav_loop: arm finger detect, then poll the TA every ~6.7 ms while the
// finger is down, turning each reported gesture into a key; after a gesture (or 2 s of resting)
// wait for the finger to lift before arming again.
void FpcEngine::navigate(CancelToken& cancel) {
    using clock = std::chrono::steady_clock;
    constexpr auto kPollPeriod = std::chrono::microseconds(6666);
    constexpr auto kHoldLimit = std::chrono::seconds(2);

    if (!mReady || !mNavOk) {
        cancel.sleepMs(60 * 1000);
        return;
    }
    for (;;) {  // finger must be lifted first
        int32_t res = 0;
        if (mTa.fingerLost(&res) || res < 0) {
            cancel.sleepMs(1000);
            return;
        }
        if (res > 0) break;
        if (cancel.sleepMs(kFingerLostPollMs)) return;
    }
    if (int rc = mTa.navStart()) {
        LOG(ERROR) << "nav_start: " << rc;
        cancel.sleepMs(5000);
        return;
    }
    bool stop = false;
    while (!stop && !cancel.cancelled()) {
        if (mTa.navArm() != 0) break;
        FpcSensor::Wait w = mSensor.waitIrq(cancel.fd(), -1);
        if (w != FpcSensor::Wait::IRQ) break;
        mSensor.ackIrq();

        const auto down = clock::now();
        auto last = clock::time_point{};
        bool fired = false;
        for (;;) {
            if (cancel.cancelled()) {
                stop = true;
                break;
            }
            uint32_t gesture = 0;
            bool present = false;
            if (mTa.navPoll(&gesture, &present) != 0) {
                stop = true;
                break;
            }
            if (gesture) mNav.report(gesture);
            auto elapsed = clock::now() - last;
            if (elapsed < kPollPeriod) std::this_thread::sleep_for(kPollPeriod - elapsed);
            last = clock::now();
            fired = fired || gesture != 0;
            if (!present) break;  // finger lifted: arm again
            if (fired || clock::now() - down > kHoldLimit) {
                // one gesture per touch: wait for the lift
                while (!cancel.cancelled() && mTa.navPoll(&gesture, &present) == 0 && present)
                    std::this_thread::sleep_for(kPollPeriod);
                break;
            }
        }
    }
    mTa.navStop();
    if (!cancel.cancelled()) cancel.sleepMs(500);  // error exit: don't spin
}

}  // namespace angler::fpc
