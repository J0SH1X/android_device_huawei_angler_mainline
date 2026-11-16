/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <libminradio/config/RadioConfig.h>

namespace angler::radio {

class RadioConfig : public ::android::hardware::radio::minimal::RadioConfig {
  public:
    RadioConfig();

  protected:
    ::ndk::ScopedAStatus getSimSlotsStatus(int32_t serial) override;

  private:
    std::vector<::aidl::android::hardware::radio::config::SimSlotStatus> slots();
};

}  // namespace angler::radio
