/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <libminradio/sim/RadioSim.h>

#include "Qmi.h"

namespace angler::radio {

class RadioSim : public ::android::hardware::radio::minimal::RadioSim {
  public:
    RadioSim(std::shared_ptr<::android::hardware::radio::minimal::SlotContext> context);

    /* Query UIM card status, update shared state (worker thread only). */
    void refreshCardStatus(bool notify);
    /* Safe from any thread (e.g. the GLib indication handler). */
    void postRefresh() { mExec.post([this] { refreshCardStatus(true); }); }

  protected:
    void onUpdatedResponseFunctions() override;

    ::ndk::ScopedAStatus getIccCardStatus(int32_t serial) override;
    ::ndk::ScopedAStatus getImsiForApp(int32_t serial, const std::string& aid) override;
    ::ndk::ScopedAStatus iccIoForApp(
            int32_t serial, const ::aidl::android::hardware::radio::sim::IccIo& iccIo) override;
    ::ndk::ScopedAStatus supplyIccPinForApp(int32_t serial, const std::string& pin,
                                            const std::string& aid) override;
    ::ndk::ScopedAStatus supplyIccPukForApp(int32_t serial, const std::string& puk,
                                            const std::string& pin,
                                            const std::string& aid) override;
    ::ndk::ScopedAStatus getFacilityLockForApp(int32_t serial, const std::string& facility,
                                               const std::string& password, int32_t serviceClass,
                                               const std::string& appId) override;

  private:
    struct PinResult {
        ::aidl::android::hardware::radio::RadioError error;
        int retries;
    };
    PinResult pinResult(bool success, GError* err, int left, bool puk = false);
    ::aidl::android::hardware::radio::sim::IccIoResult doIccIo(
            const ::aidl::android::hardware::radio::sim::IccIo& io);
    SerialExecutor mExec{"qmiril-sim"};
};

}  // namespace angler::radio
