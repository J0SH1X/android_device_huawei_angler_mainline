/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <aidl/android/hardware/radio/voice/BnRadioVoice.h>
#include <aidl/android/hardware/radio/voice/IRadioVoiceIndication.h>
#include <aidl/android/hardware/radio/voice/IRadioVoiceResponse.h>
#include <libminradio/GuaranteedCallback.h>
#include <libminradio/RadioSlotBase.h>

#include <mutex>

#include "Qmi.h"

namespace angler::radio {

namespace aidlVoice = ::aidl::android::hardware::radio::voice;

class RadioVoice : public ::android::hardware::radio::minimal::RadioSlotBase,
                   public aidlVoice::BnRadioVoice {
  public:
    using RadioSlotBase::RadioSlotBase;
    void setupIndications();

  protected:
    using Status = ::ndk::ScopedAStatus;
    Status acceptCall(int32_t serial) override;
    Status cancelPendingUssd(int32_t serial) override;
    Status conference(int32_t serial) override;
    Status dial(int32_t serial, const aidlVoice::Dial& dialInfo) override;
    Status emergencyDial(int32_t serial, const aidlVoice::Dial& dialInfo, int32_t categories,
                         const std::vector<std::string>& urns,
                         aidlVoice::EmergencyCallRouting routing, bool hasKnownUserIntentEmergency,
                         bool isTesting) override;
    Status exitEmergencyCallbackMode(int32_t serial) override;
    Status explicitCallTransfer(int32_t serial) override;
    Status getCallForwardStatus(int32_t serial, const aidlVoice::CallForwardInfo& callInfo) override;
    Status getCallWaiting(int32_t serial, int32_t serviceClass) override;
    Status getClip(int32_t serial) override;
    Status getClir(int32_t serial) override;
    Status getCurrentCalls(int32_t serial) override;
    Status getLastCallFailCause(int32_t serial) override;
    Status getMute(int32_t serial) override;
    Status getPreferredVoicePrivacy(int32_t serial) override;
    Status getTtyMode(int32_t serial) override;
    Status handleStkCallSetupRequestFromSim(int32_t serial, bool accept) override;
    Status hangup(int32_t serial, int32_t gsmIndex) override;
    Status hangupForegroundResumeBackground(int32_t serial) override;
    Status hangupWaitingOrBackground(int32_t serial) override;
    Status isVoNrEnabled(int32_t serial) override;
    Status rejectCall(int32_t serial) override;
    Status responseAcknowledgement() override;
    Status sendBurstDtmf(int32_t serial, const std::string& dtmf, int32_t on, int32_t off) override;
    Status sendCdmaFeatureCode(int32_t serial, const std::string& featureCode) override;
    Status sendDtmf(int32_t serial, const std::string& s) override;
    Status sendUssd(int32_t serial, const std::string& ussd) override;
    Status separateConnection(int32_t serial, int32_t gsmIndex) override;
    Status setCallForward(int32_t serial, const aidlVoice::CallForwardInfo& callInfo) override;
    Status setCallWaiting(int32_t serial, bool enable, int32_t serviceClass) override;
    Status setClir(int32_t serial, int32_t status) override;
    Status setMute(int32_t serial, bool enable) override;
    Status setPreferredVoicePrivacy(int32_t serial, bool enable) override;
    Status setResponseFunctions(
            const std::shared_ptr<aidlVoice::IRadioVoiceResponse>& response,
            const std::shared_ptr<aidlVoice::IRadioVoiceIndication>& indication) override;
    Status setTtyMode(int32_t serial, aidlVoice::TtyMode mode) override;
    Status setVoNrEnabled(int32_t serial, bool enable) override;
    Status startDtmf(int32_t serial, const std::string& s) override;
    Status stopDtmf(int32_t serial) override;
    Status switchWaitingOrHoldingAndActive(int32_t serial) override;

    ::android::hardware::radio::minimal::GuaranteedCallback<
            aidlVoice::IRadioVoiceIndication, aidlVoice::IRadioVoiceIndicationDefault, true>
            indicate;
    ::android::hardware::radio::minimal::GuaranteedCallback<
            aidlVoice::IRadioVoiceResponse, aidlVoice::IRadioVoiceResponseDefault>
            respond;

  private:
    bool queryCalls(std::vector<aidlVoice::Call>* calls);
    int findCall(std::initializer_list<int32_t> states);
    bool dialNumber(const std::string& number);
    bool endCall(int id);
    bool manageCalls(QmiVoiceSupplementaryServiceType type);
    bool dtmf(int callId, char digit, bool start);

    SerialExecutor mExec{"qmiril-voice"};
    void setupUssdIndications();
    void ussdIndication(aidlVoice::UssdModeType mode, const std::string& msg);
    std::mutex mUssdLock;
    bool mUssdActive = false;         /* a USSD session is open */
    bool mUssdWaitingForUser = false; /* network asked for a reply (REQUEST) */
    bool mMute = false;
};

}  // namespace angler::radio
