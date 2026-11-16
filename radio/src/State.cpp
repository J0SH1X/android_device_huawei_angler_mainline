/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "State.h"

namespace angler::radio {

State& State::get() {
    static State instance;
    return instance;
}

SimSnapshot State::sim() const {
    std::lock_guard lk(mLock);
    return mSim;
}

void State::setSim(const SimSnapshot& s) {
    std::lock_guard lk(mLock);
    std::string iccid = mSim.card.iccid;
    mSim = s;
    if (mSim.card.iccid.empty()) mSim.card.iccid = iccid;
}

void State::setIccid(const std::string& iccid) {
    std::lock_guard lk(mLock);
    mSim.card.iccid = iccid;
}

void State::addSimListener(std::function<void()> fn) {
    std::lock_guard lk(mLock);
    mSimListeners.push_back(std::move(fn));
}

void State::notifySimChanged() {
    std::vector<std::function<void()>> fns;
    {
        std::lock_guard lk(mLock);
        fns = mSimListeners;
    }
    for (auto& fn : fns) fn();
}

}  // namespace angler::radio
