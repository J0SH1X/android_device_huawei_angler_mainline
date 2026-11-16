/*
 * State shared between the radio HAL interfaces.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <aidl/android/hardware/radio/sim/CardStatus.h>

namespace angler::radio {

struct SimSnapshot {
    bool valid = false;
    ::aidl::android::hardware::radio::sim::CardStatus card{};
    std::string gwAidHex; /* AID of the GW provisioning app (USIM/SIM) */
    int pin1Retries = -1;
    int puk1Retries = -1;
};

class State {
  public:
    static State& get();

    SimSnapshot sim() const;
    void setSim(const SimSnapshot& s);
    void setIccid(const std::string& iccid);

    /* Listeners fire on whichever thread calls notify*() */
    void addSimListener(std::function<void()> fn);
    void notifySimChanged();

  private:
    mutable std::mutex mLock;
    SimSnapshot mSim;
    std::vector<std::function<void()>> mSimListeners;
};

}  // namespace angler::radio
