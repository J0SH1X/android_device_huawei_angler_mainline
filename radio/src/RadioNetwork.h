/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <libminradio/network/RadioNetwork.h>

#include "Qmi.h"

namespace angler::radio {

class RadioNetwork : public ::android::hardware::radio::minimal::RadioNetwork {
  public:
    using ::android::hardware::radio::minimal::RadioNetwork::RadioNetwork;

    /* Register NAS indications (call once the modem is ready, off the GLib thread). */
    void setupIndications();

  protected:
    ::ndk::ScopedAStatus getVoiceRegistrationState(int32_t serial) override;
    ::ndk::ScopedAStatus getDataRegistrationState(int32_t serial) override;
    ::ndk::ScopedAStatus getVoiceRadioTechnology(int32_t serial) override;
    ::ndk::ScopedAStatus getSignalStrength(int32_t serial) override;
    ::ndk::ScopedAStatus getNetworkSelectionMode(int32_t serial) override;
    ::ndk::ScopedAStatus setNetworkSelectionModeAutomatic(int32_t serial) override;

  private:
    struct Serving {
        bool ok = false;
        QmiNasRegistrationState reg = QMI_NAS_REGISTRATION_STATE_UNKNOWN;
        QmiNasAttachState cs = QMI_NAS_ATTACH_STATE_UNKNOWN;
        QmiNasAttachState ps = QMI_NAS_ATTACH_STATE_UNKNOWN;
        QmiNasRadioInterface rat = QMI_NAS_RADIO_INTERFACE_NONE;
        bool roaming = false;
        std::string mcc, mnc, name;
        uint32_t cid = 0;
        uint16_t lac = 0, tac = 0;
    };
    Serving queryServing();
    ::aidl::android::hardware::radio::network::RegStateResult toRegState(const Serving& s,
                                                                        bool data);
    ::aidl::android::hardware::radio::network::SignalStrength querySignal();

    SerialExecutor mExec{"qmiril-net"};
};

}  // namespace angler::radio
