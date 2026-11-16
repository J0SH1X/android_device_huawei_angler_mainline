/*
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "qmiril"

#include "RadioConfig.h"

#include <libminradio/debug.h>
#include <libminradio/response.h>

#include "State.h"

#define RADIO_MODULE "Config"

namespace angler::radio {

using ::aidl::android::hardware::radio::RadioIndicationType;
using ::android::hardware::radio::minimal::noError;
using ::ndk::ScopedAStatus;
namespace aidl = ::aidl::android::hardware::radio::config;
namespace aidlSim = ::aidl::android::hardware::radio::sim;

RadioConfig::RadioConfig() {
    State::get().addSimListener([this] {
        indicate()->simSlotsStatusChanged(RadioIndicationType::UNSOLICITED, slots());
    });
}

std::vector<aidl::SimSlotStatus> RadioConfig::slots() {
    auto sim = State::get().sim();
    aidl::SimSlotStatus slot{
            .cardState = sim.valid ? sim.card.cardState : aidlSim::CardStatus::STATE_ABSENT,
            .atr = "",
            .eid = "",
            .portInfo = {{
                    .iccId = sim.card.iccid,
                    .logicalSlotId = 0,
                    .portActive = true,
            }},
    };
    return {slot};
}

ScopedAStatus RadioConfig::getSimSlotsStatus(int32_t serial) {
    LOG_CALL;
    respond()->getSimSlotsStatusResponse(noError(serial), slots());
    return ScopedAStatus::ok();
}

}  // namespace angler::radio
