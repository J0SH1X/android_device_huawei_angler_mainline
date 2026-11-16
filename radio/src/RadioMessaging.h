/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <aidl/android/hardware/radio/messaging/BnRadioMessaging.h>
#include <aidl/android/hardware/radio/messaging/IRadioMessagingIndication.h>
#include <aidl/android/hardware/radio/messaging/IRadioMessagingResponse.h>
#include <libminradio/GuaranteedCallback.h>
#include <libminradio/RadioSlotBase.h>

#include "Qmi.h"

namespace angler::radio {

namespace aidlMsg = ::aidl::android::hardware::radio::messaging;

class RadioMessaging : public ::android::hardware::radio::minimal::RadioSlotBase,
                       public aidlMsg::BnRadioMessaging {
  public:
    using RadioSlotBase::RadioSlotBase;
    void setupIndications();

  protected:
    using Status = ::ndk::ScopedAStatus;
    Status acknowledgeIncomingGsmSmsWithPdu(int32_t serial, bool success,
                                            const std::string& ackPdu) override;
    Status acknowledgeLastIncomingCdmaSms(int32_t serial, const aidlMsg::CdmaSmsAck& smsAck) override;
    Status acknowledgeLastIncomingGsmSms(int32_t serial, bool success,
                                         aidlMsg::SmsAcknowledgeFailCause cause) override;
    Status deleteSmsOnRuim(int32_t serial, int32_t index) override;
    Status deleteSmsOnSim(int32_t serial, int32_t index) override;
    Status getCdmaBroadcastConfig(int32_t serial) override;
    Status getGsmBroadcastConfig(int32_t serial) override;
    Status getSmscAddress(int32_t serial) override;
    Status reportSmsMemoryStatus(int32_t serial, bool available) override;
    Status responseAcknowledgement() override;
    Status sendCdmaSms(int32_t serial, const aidlMsg::CdmaSmsMessage& sms) override;
    Status sendCdmaSmsExpectMore(int32_t serial, const aidlMsg::CdmaSmsMessage& sms) override;
    Status sendImsSms(int32_t serial, const aidlMsg::ImsSmsMessage& message) override;
    Status sendSms(int32_t serial, const aidlMsg::GsmSmsMessage& message) override;
    Status sendSmsExpectMore(int32_t serial, const aidlMsg::GsmSmsMessage& message) override;
    Status setCdmaBroadcastActivation(int32_t serial, bool activate) override;
    Status setCdmaBroadcastConfig(
            int32_t serial, const std::vector<aidlMsg::CdmaBroadcastSmsConfigInfo>& configInfo) override;
    Status setGsmBroadcastActivation(int32_t serial, bool activate) override;
    Status setGsmBroadcastConfig(
            int32_t serial, const std::vector<aidlMsg::GsmBroadcastSmsConfigInfo>& configInfo) override;
    Status setResponseFunctions(
            const std::shared_ptr<aidlMsg::IRadioMessagingResponse>& response,
            const std::shared_ptr<aidlMsg::IRadioMessagingIndication>& indication) override;
    Status setSmscAddress(int32_t serial, const std::string& smsc) override;
    Status writeSmsToRuim(int32_t serial, const aidlMsg::CdmaSmsWriteArgs& cdmaSms) override;
    Status writeSmsToSim(int32_t serial, const aidlMsg::SmsWriteArgs& smsWriteArgs) override;

    ::android::hardware::radio::minimal::GuaranteedCallback<
            aidlMsg::IRadioMessagingIndication, aidlMsg::IRadioMessagingIndicationDefault, true>
            indicate;
    ::android::hardware::radio::minimal::GuaranteedCallback<
            aidlMsg::IRadioMessagingResponse, aidlMsg::IRadioMessagingResponseDefault>
            respond;

  private:
    void doSend(int32_t serial, const aidlMsg::GsmSmsMessage& message, bool expectMore);
    void ack(bool success);
    void readStored(QmiWmsStorageType storage, guint32 index);

    SerialExecutor mExec{"qmiril-sms"};
    std::mutex mAckLock;
    bool mAckPending = false;   /* a transfer-route MT message needs a WMS ACK */
    guint32 mAckTransaction = 0;
};

}  // namespace angler::radio
