/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <libminradio/modem/RadioModem.h>

#include "Qmi.h"

namespace angler::radio {

class RadioModem : public ::android::hardware::radio::minimal::RadioModem {
  public:
    RadioModem(std::shared_ptr<::android::hardware::radio::minimal::SlotContext> context);

  protected:
    void onUpdatedResponseFunctions() override;
    ::ndk::ScopedAStatus getImei(int32_t serial) override;
    ::ndk::ScopedAStatus getBasebandVersion(int32_t serial) override;
    ::ndk::ScopedAStatus setRadioPower(int32_t serial, bool powerOn, bool forEmergencyCall,
                                       bool preferredForEmergencyCall) override;

  private:
    ::aidl::android::hardware::radio::modem::RadioState currentRadioState();
    SerialExecutor mExec{"qmiril-modem"};
};

}  // namespace angler::radio
