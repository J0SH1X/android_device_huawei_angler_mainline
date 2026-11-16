/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "fpc-angler-session"

#include <android-base/logging.h>
#include <aidl/android/hardware/biometrics/common/BnCancellationSignal.h>

#include "Fingerprint.h"

namespace angler::fpc {

namespace {
class CancelSignal : public common::BnCancellationSignal {
  public:
    explicit CancelSignal(std::shared_ptr<CancelToken> t) : mToken(std::move(t)) {}
    ndk::ScopedAStatus cancel() override {
        mToken->trigger();
        return ndk::ScopedAStatus::ok();
    }

  private:
    std::shared_ptr<CancelToken> mToken;
};
}  // namespace

Session::Session(int userId, std::shared_ptr<fp::ISessionCallback> cb, FpcEngine* engine,
                 Worker* worker)
    : mUserId(userId), mCb(std::move(cb)), mEngine(engine), mWorker(worker) {
    // Queued ahead of every operation of this session, so the TA is on the right user's db.
    mWorker->post([e = mEngine, u = mUserId] { e->selectUser(u); });
}

std::shared_ptr<common::ICancellationSignal> Session::makeSignal(std::shared_ptr<CancelToken> t) {
    return ndk::SharedRefBase::make<CancelSignal>(std::move(t));
}

ndk::ScopedAStatus Session::generateChallenge() {
    mWorker->post([e = mEngine, cb = mCb] { e->generateChallenge(*cb); });
    return ok();
}

ndk::ScopedAStatus Session::revokeChallenge(int64_t challenge) {
    mWorker->post([e = mEngine, cb = mCb, challenge] { e->revokeChallenge(*cb, challenge); });
    return ok();
}

ndk::ScopedAStatus Session::enroll(const keymaster::HardwareAuthToken& hat,
                                   std::shared_ptr<common::ICancellationSignal>* out) {
    auto token = std::make_shared<CancelToken>();
    mWorker->post([e = mEngine, cb = mCb, hat, token] { e->enroll(*cb, hat, *token); });
    *out = makeSignal(token);
    return ok();
}

ndk::ScopedAStatus Session::authenticate(int64_t operationId,
                                         std::shared_ptr<common::ICancellationSignal>* out) {
    auto token = std::make_shared<CancelToken>();
    mWorker->post(
            [e = mEngine, cb = mCb, operationId, token] { e->authenticate(*cb, operationId, *token); });
    *out = makeSignal(token);
    return ok();
}

ndk::ScopedAStatus Session::detectInteraction(std::shared_ptr<common::ICancellationSignal>* out) {
    auto token = std::make_shared<CancelToken>();
    mWorker->post([e = mEngine, cb = mCb, token] { e->detectInteraction(*cb, *token); });
    *out = makeSignal(token);
    return ok();
}

ndk::ScopedAStatus Session::enumerateEnrollments() {
    mWorker->post([e = mEngine, cb = mCb] { e->enumerateEnrollments(*cb); });
    return ok();
}

ndk::ScopedAStatus Session::removeEnrollments(const std::vector<int32_t>& ids) {
    mWorker->post([e = mEngine, cb = mCb, ids] { e->removeEnrollments(*cb, ids); });
    return ok();
}

ndk::ScopedAStatus Session::getAuthenticatorId() {
    mWorker->post([e = mEngine, cb = mCb] { e->getAuthenticatorId(*cb); });
    return ok();
}

ndk::ScopedAStatus Session::invalidateAuthenticatorId() {
    mWorker->post([e = mEngine, cb = mCb] { e->invalidateAuthenticatorId(*cb); });
    return ok();
}

ndk::ScopedAStatus Session::resetLockout(const keymaster::HardwareAuthToken& hat) {
    mWorker->post([e = mEngine, cb = mCb, hat] { e->resetLockout(*cb, hat); });
    return ok();
}

ndk::ScopedAStatus Session::close() {
    mWorker->post([cb = mCb] { cb->onSessionClosed(); });
    return ok();
}

}  // namespace angler::fpc
