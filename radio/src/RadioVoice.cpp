/*
 * Voice: QMI VOICE service, CS calls only (no VoLTE/IMS).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "qmiril"

#include "RadioVoice.h"

#include <libminradio/debug.h>
#include <libminradio/response.h>

#define RADIO_MODULE "Voice"

namespace angler::radio {

using ::aidl::android::hardware::radio::RadioError;
using ::aidl::android::hardware::radio::RadioIndicationType;
using ::android::hardware::radio::minimal::errorResponse;
using ::android::hardware::radio::minimal::noError;
using ::android::hardware::radio::minimal::notSupported;
using Status = ::ndk::ScopedAStatus;
constexpr auto ok = &Status::ok;

static int32_t mapState(QmiVoiceCallState s) {
    switch (s) {
        case QMI_VOICE_CALL_STATE_ORIGINATION:
        case QMI_VOICE_CALL_STATE_CC_IN_PROGRESS:
        case QMI_VOICE_CALL_STATE_SETUP:
            return aidlVoice::Call::STATE_DIALING;
        case QMI_VOICE_CALL_STATE_ALERTING:
            return aidlVoice::Call::STATE_ALERTING;
        case QMI_VOICE_CALL_STATE_CONVERSATION:
            return aidlVoice::Call::STATE_ACTIVE;
        case QMI_VOICE_CALL_STATE_HOLD:
            return aidlVoice::Call::STATE_HOLDING;
        case QMI_VOICE_CALL_STATE_INCOMING:
            return aidlVoice::Call::STATE_INCOMING;
        case QMI_VOICE_CALL_STATE_WAITING:
            return aidlVoice::Call::STATE_WAITING;
        default:
            return -1; /* ended / disconnecting / unknown: not reported */
    }
}

static int32_t mapPresentation(QmiVoicePresentation p) {
    switch (p) {
        case QMI_VOICE_PRESENTATION_ALLOWED:
            return aidlVoice::Call::PRESENTATION_ALLOWED;
        case QMI_VOICE_PRESENTATION_RESTRICTED:
            return aidlVoice::Call::PRESENTATION_RESTRICTED;
        case QMI_VOICE_PRESENTATION_PAYPHONE:
            return aidlVoice::Call::PRESENTATION_PAYPHONE;
        default:
            return aidlVoice::Call::PRESENTATION_UNKNOWN;
    }
}

/* Message and indication carry identically laid out (but distinct) structs. */
template <typename InfoElement, typename NumberElement>
static std::vector<aidlVoice::Call> buildCalls(GArray* info, GArray* numbers) {
    std::vector<aidlVoice::Call> calls;
    for (guint i = 0; info && i < info->len; i++) {
        auto& c = g_array_index(info, InfoElement, i);
        int32_t state = mapState(c.state);
        if (state < 0) continue;
        aidlVoice::Call call{};
        call.state = state;
        call.index = c.id;
        call.isMpty = false;
        call.isMT = c.direction == QMI_VOICE_CALL_DIRECTION_MT;
        call.als = 0;
        call.isVoice = c.type == QMI_VOICE_CALL_TYPE_VOICE ||
                       c.type == QMI_VOICE_CALL_TYPE_EMERGENCY;
        call.numberPresentation = aidlVoice::Call::PRESENTATION_UNKNOWN;
        call.namePresentation = aidlVoice::Call::PRESENTATION_UNKNOWN;
        for (guint j = 0; numbers && j < numbers->len; j++) {
            auto& n = g_array_index(numbers, NumberElement, j);
            if (n.id != c.id) continue;
            call.number = n.type ? n.type : ""; /* libqmi names the number field "type" */
            call.numberPresentation = mapPresentation(n.presentation_indicator);
        }
        call.toa = (!call.number.empty() && call.number[0] == '+') ? 145 : 129;
        calls.push_back(std::move(call));
    }
    return calls;
}

void RadioVoice::setupIndications() {
    auto& m = QmiModem::get();
    if (!m.voice) {
        LOG(ERROR) << "no VOICE client, calls unavailable";
        return;
    }
    g_autoptr(GError) err = nullptr;
    auto* in = qmi_message_voice_indication_register_input_new();
    qmi_message_voice_indication_register_input_set_call_notification_events(in, TRUE, nullptr);
    qmi_message_voice_indication_register_input_set_ussd_notification_events(in, TRUE, nullptr);
    auto out = QMI_SYNC(voice, indication_register, m.voice, in, &err);
    qmi_message_voice_indication_register_input_unref(in);
    if (!out) LOG(WARNING) << "voice indication_register: " << err->message;

    m.onIndication<QmiIndicationVoiceAllCallStatusOutput>(
            QMI_CLIENT(m.voice), "all-call-status",
            [this](QmiIndicationVoiceAllCallStatusOutput* o) {
                GArray *info = nullptr, *numbers = nullptr;
                qmi_indication_voice_all_call_status_output_get_call_information(o, &info, nullptr);
                qmi_indication_voice_all_call_status_output_get_remote_party_number(o, &numbers,
                                                                                   nullptr);
                auto calls = buildCalls<QmiIndicationVoiceAllCallStatusOutputCallInformationCall,
                                        QmiIndicationVoiceAllCallStatusOutputRemotePartyNumberCall>(
                        info, numbers);
                indicate()->callStateChanged(RadioIndicationType::UNSOLICITED);
                for (auto& c : calls) {
                    if (c.state == aidlVoice::Call::STATE_INCOMING) {
                        indicate()->callRing(RadioIndicationType::UNSOLICITED, true, {});
                        break;
                    }
                }
            });
    setupUssdIndications();
}

bool RadioVoice::queryCalls(std::vector<aidlVoice::Call>* calls) {
    g_autoptr(GError) err = nullptr;
    auto out = QMI_SYNC(voice, get_all_call_info, QmiModem::get().voice, nullptr, &err);
    if (!out) {
        LOG(ERROR) << "get_all_call_info: " << err->message;
        return false;
    }
    GArray *info = nullptr, *numbers = nullptr;
    qmi_message_voice_get_all_call_info_output_get_call_information(out.get(), &info, nullptr);
    qmi_message_voice_get_all_call_info_output_get_remote_party_number(out.get(), &numbers, nullptr);
    *calls = buildCalls<QmiMessageVoiceGetAllCallInfoOutputCallInformationCall,
                        QmiMessageVoiceGetAllCallInfoOutputRemotePartyNumberCall>(info, numbers);
    return true;
}

int RadioVoice::findCall(std::initializer_list<int32_t> states) {
    std::vector<aidlVoice::Call> calls;
    if (!queryCalls(&calls)) return -1;
    for (int32_t st : states)
        for (auto& c : calls)
            if (c.state == st) return c.index;
    return -1;
}

bool RadioVoice::dialNumber(const std::string& number) {
    g_autoptr(GError) err = nullptr;
    auto* in = qmi_message_voice_dial_call_input_new();
    qmi_message_voice_dial_call_input_set_calling_number(in, number.c_str(), nullptr);
    auto out = QMI_SYNC(voice, dial_call, QmiModem::get().voice, in, &err);
    qmi_message_voice_dial_call_input_unref(in);
    if (!out) {
        LOG(ERROR) << "dial_call: " << err->message;
        return false;
    }
    return true;
}

bool RadioVoice::endCall(int id) {
    g_autoptr(GError) err = nullptr;
    auto* in = qmi_message_voice_end_call_input_new();
    qmi_message_voice_end_call_input_set_call_id(in, id, nullptr);
    auto out = QMI_SYNC(voice, end_call, QmiModem::get().voice, in, &err);
    qmi_message_voice_end_call_input_unref(in);
    if (!out) LOG(ERROR) << "end_call " << id << ": " << err->message;
    return out != nullptr;
}

bool RadioVoice::manageCalls(QmiVoiceSupplementaryServiceType type) {
    g_autoptr(GError) err = nullptr;
    auto* in = qmi_message_voice_manage_calls_input_new();
    qmi_message_voice_manage_calls_input_set_service_type(in, type, nullptr);
    auto out = QMI_SYNC(voice, manage_calls, QmiModem::get().voice, in, &err);
    qmi_message_voice_manage_calls_input_unref(in);
    if (!out) LOG(ERROR) << "manage_calls " << type << ": " << err->message;
    return out != nullptr;
}

bool RadioVoice::dtmf(int callId, char digit, bool start) {
    g_autoptr(GError) err = nullptr;
    if (start) {
        auto* in = qmi_message_voice_start_continuous_dtmf_input_new();
        qmi_message_voice_start_continuous_dtmf_input_set_data(in, callId, digit, nullptr);
        auto out = QMI_SYNC(voice, start_continuous_dtmf, QmiModem::get().voice, in, &err);
        qmi_message_voice_start_continuous_dtmf_input_unref(in);
        return out != nullptr;
    }
    auto* in = qmi_message_voice_stop_continuous_dtmf_input_new();
    qmi_message_voice_stop_continuous_dtmf_input_set_data(in, callId, nullptr);
    auto out = QMI_SYNC(voice, stop_continuous_dtmf, QmiModem::get().voice, in, &err);
    qmi_message_voice_stop_continuous_dtmf_input_unref(in);
    return out != nullptr;
}

#define POST(...) mExec.post([=, this] { __VA_ARGS__; })
#define RESULT(okay) ((okay) ? noError(serial) : errorResponse(serial, RadioError::GENERIC_FAILURE))

Status RadioVoice::getCurrentCalls(int32_t serial) {
    LOG_CALL;
    POST({
        std::vector<aidlVoice::Call> calls;
        bool good = queryCalls(&calls);
        respond()->getCurrentCallsResponse(RESULT(good), calls);
    });
    return ok();
}

Status RadioVoice::dial(int32_t serial, const aidlVoice::Dial& dialInfo) {
    LOG_CALL << dialInfo.address.size() << " digits";
    POST(respond()->dialResponse(RESULT(dialNumber(dialInfo.address))));
    return ok();
}

Status RadioVoice::emergencyDial(int32_t serial, const aidlVoice::Dial& dialInfo, int32_t, const std::vector<std::string>&,
                                 aidlVoice::EmergencyCallRouting, bool, bool) {
    LOG_CALL;
    POST(respond()->emergencyDialResponse(RESULT(dialNumber(dialInfo.address))));
    return ok();
}

Status RadioVoice::hangup(int32_t serial, int32_t gsmIndex) {
    LOG_CALL << gsmIndex;
    POST(respond()->hangupConnectionResponse(RESULT(endCall(gsmIndex))));
    return ok();
}

Status RadioVoice::acceptCall(int32_t serial) {
    LOG_CALL;
    POST({
        int id = findCall({aidlVoice::Call::STATE_INCOMING, aidlVoice::Call::STATE_WAITING});
        bool good = false;
        if (id >= 0) {
            g_autoptr(GError) err = nullptr;
            auto* in = qmi_message_voice_answer_call_input_new();
            qmi_message_voice_answer_call_input_set_call_id(in, id, nullptr);
            auto out = QMI_SYNC(voice, answer_call, QmiModem::get().voice, in, &err);
            qmi_message_voice_answer_call_input_unref(in);
            if (!out) LOG(ERROR) << "answer_call: " << err->message;
            good = out != nullptr;
        }
        respond()->acceptCallResponse(RESULT(good));
    });
    return ok();
}

Status RadioVoice::rejectCall(int32_t serial) {
    LOG_CALL;
    POST({
        int id = findCall({aidlVoice::Call::STATE_INCOMING, aidlVoice::Call::STATE_WAITING});
        respond()->rejectCallResponse(RESULT(id >= 0 && endCall(id)));
    });
    return ok();
}

Status RadioVoice::hangupWaitingOrBackground(int32_t serial) {
    LOG_CALL;
    POST({
        int id = findCall({aidlVoice::Call::STATE_INCOMING});
        bool good = id >= 0 ? endCall(id)
                            : manageCalls(QMI_VOICE_SUPPLEMENTARY_SERVICE_TYPE_RELEASE_HELD_OR_WAITING);
        respond()->hangupWaitingOrBackgroundResponse(RESULT(good));
    });
    return ok();
}

Status RadioVoice::hangupForegroundResumeBackground(int32_t serial) {
    LOG_CALL;
    POST(respond()->hangupForegroundResumeBackgroundResponse(RESULT(manageCalls(
            QMI_VOICE_SUPPLEMENTARY_SERVICE_TYPE_RELEASE_ACTIVE_ACCEPT_HELD_OR_WAITING))));
    return ok();
}

Status RadioVoice::switchWaitingOrHoldingAndActive(int32_t serial) {
    LOG_CALL;
    POST(respond()->switchWaitingOrHoldingAndActiveResponse(RESULT(
            manageCalls(QMI_VOICE_SUPPLEMENTARY_SERVICE_TYPE_HOLD_ACTIVE_ACCEPT_WAITING_OR_HELD))));
    return ok();
}

Status RadioVoice::conference(int32_t serial) {
    LOG_CALL;
    POST(respond()->conferenceResponse(
            RESULT(manageCalls(QMI_VOICE_SUPPLEMENTARY_SERVICE_TYPE_MAKE_CONFERENCE_CALL))));
    return ok();
}

Status RadioVoice::explicitCallTransfer(int32_t serial) {
    LOG_CALL;
    POST(respond()->explicitCallTransferResponse(
            RESULT(manageCalls(QMI_VOICE_SUPPLEMENTARY_SERVICE_TYPE_EXPLICIT_CALL_TRANSFER))));
    return ok();
}

Status RadioVoice::sendDtmf(int32_t serial, const std::string& s) {
    LOG_CALL << s.size();
    POST({
        int id = findCall({aidlVoice::Call::STATE_ACTIVE});
        bool good = id >= 0;
        for (char c : s) {
            if (!good) break;
            good = dtmf(id, c, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            good = dtmf(id, c, false) && good;
        }
        respond()->sendDtmfResponse(RESULT(good));
    });
    return ok();
}

Status RadioVoice::startDtmf(int32_t serial, const std::string& s) {
    LOG_CALL;
    POST({
        int id = findCall({aidlVoice::Call::STATE_ACTIVE});
        respond()->startDtmfResponse(RESULT(id >= 0 && !s.empty() && dtmf(id, s[0], true)));
    });
    return ok();
}

Status RadioVoice::stopDtmf(int32_t serial) {
    LOG_CALL;
    POST({
        int id = findCall({aidlVoice::Call::STATE_ACTIVE});
        respond()->stopDtmfResponse(RESULT(id >= 0 && dtmf(id, 0, false)));
    });
    return ok();
}

Status RadioVoice::getLastCallFailCause(int32_t serial) {
    LOG_CALL;
    respond()->getLastCallFailCauseResponse(
            noError(serial), {.causeCode = aidlVoice::LastCallFailCause::NORMAL, .vendorCause = ""});
    return ok();
}

Status RadioVoice::getMute(int32_t serial) {
    respond()->getMuteResponse(noError(serial), mMute);
    return ok();
}

Status RadioVoice::setMute(int32_t serial, bool enable) {
    mMute = enable;
    respond()->setMuteResponse(noError(serial));
    return ok();
}

Status RadioVoice::getClir(int32_t serial) {
    respond()->getClirResponse(noError(serial), 0 /* default */, 2 /* unknown */);
    return ok();
}

Status RadioVoice::setClir(int32_t serial, int32_t) {
    respond()->setClirResponse(noError(serial));
    return ok();
}

Status RadioVoice::getPreferredVoicePrivacy(int32_t serial) {
    respond()->getPreferredVoicePrivacyResponse(noError(serial), false);
    return ok();
}

Status RadioVoice::setPreferredVoicePrivacy(int32_t serial, bool) {
    respond()->setPreferredVoicePrivacyResponse(noError(serial));
    return ok();
}

Status RadioVoice::getTtyMode(int32_t serial) {
    respond()->getTtyModeResponse(noError(serial), aidlVoice::TtyMode::OFF);
    return ok();
}

Status RadioVoice::setTtyMode(int32_t serial, aidlVoice::TtyMode) {
    respond()->setTtyModeResponse(noError(serial));
    return ok();
}

Status RadioVoice::isVoNrEnabled(int32_t serial) {
    respond()->isVoNrEnabledResponse(noError(serial), false);
    return ok();
}

Status RadioVoice::setVoNrEnabled(int32_t serial, bool) {
    respond()->setVoNrEnabledResponse(notSupported(serial));
    return ok();
}

Status RadioVoice::exitEmergencyCallbackMode(int32_t serial) {
    respond()->exitEmergencyCallbackModeResponse(noError(serial));
    return ok();
}

/* Supplementary services: not implemented yet (USSD further below) */
Status RadioVoice::getCallForwardStatus(int32_t serial, const aidlVoice::CallForwardInfo&) {
    respond()->getCallForwardStatusResponse(notSupported(serial), {});
    return ok();
}
Status RadioVoice::setCallForward(int32_t serial, const aidlVoice::CallForwardInfo&) {
    respond()->setCallForwardResponse(notSupported(serial));
    return ok();
}
Status RadioVoice::getCallWaiting(int32_t serial, int32_t) {
    respond()->getCallWaitingResponse(notSupported(serial), false, 0);
    return ok();
}
Status RadioVoice::setCallWaiting(int32_t serial, bool, int32_t) {
    respond()->setCallWaitingResponse(notSupported(serial));
    return ok();
}
Status RadioVoice::getClip(int32_t serial) {
    respond()->getClipResponse(notSupported(serial), aidlVoice::ClipStatus::UNKNOWN);
    return ok();
}
Status RadioVoice::separateConnection(int32_t serial, int32_t) {
    respond()->separateConnectionResponse(notSupported(serial));
    return ok();
}
Status RadioVoice::handleStkCallSetupRequestFromSim(int32_t serial, bool) {
    respond()->handleStkCallSetupRequestFromSimResponse(notSupported(serial));
    return ok();
}
Status RadioVoice::sendBurstDtmf(int32_t serial, const std::string&, int32_t, int32_t) {
    respond()->sendBurstDtmfResponse(notSupported(serial));
    return ok();
}
Status RadioVoice::sendCdmaFeatureCode(int32_t serial, const std::string&) {
    respond()->sendCdmaFeatureCodeResponse(notSupported(serial));
    return ok();
}

/* ---------------- USSD ---------------- */

static std::string ussUtf16ToUtf8(GArray* a) {
    if (!a || !a->len) return {};
    g_autofree gchar* u =
            g_utf16_to_utf8(reinterpret_cast<const gunichar2*>(a->data), a->len, nullptr, nullptr, nullptr);
    return u ? u : "";
}

/* uss_data TLV: ASCII / 8-bit as is, UCS2 big endian */
static std::string ussDataToUtf8(QmiVoiceUssDataCodingScheme dcs, GArray* a) {
    if (!a || !a->len) return {};
    auto* p = reinterpret_cast<const guint8*>(a->data);
    if (dcs == QMI_VOICE_USS_DATA_CODING_SCHEME_UCS2) {
        std::vector<gunichar2> w;
        for (guint i = 0; i + 1 < a->len; i += 2) w.push_back(gunichar2(p[i] << 8 | p[i + 1]));
        g_autofree gchar* u = g_utf16_to_utf8(w.data(), w.size(), nullptr, nullptr, nullptr);
        return u ? u : "";
    }
    return std::string(reinterpret_cast<const char*>(p), a->len);
}

void RadioVoice::ussdIndication(aidlVoice::UssdModeType mode, const std::string& msg) {
    {
        std::lock_guard lk(mUssdLock);
        mUssdWaitingForUser = mode == aidlVoice::UssdModeType::REQUEST;
        if (mode != aidlVoice::UssdModeType::REQUEST) mUssdActive = false;
    }
    LOG(INFO) << "USSD -> Android, mode " << static_cast<int>(mode) << ", " << msg.size() << " chars";
    indicate()->onUssd(RadioIndicationType::UNSOLICITED, mode, msg);
}

void RadioVoice::setupUssdIndications() {
    auto& m = QmiModem::get();
    /* answer to our ORIGINATE_USSD_NO_WAIT */
    m.onIndication<QmiIndicationVoiceOriginateUssdNoWaitOutput>(
            QMI_CLIENT(m.voice), "originate-ussd-no-wait",
            [this](QmiIndicationVoiceOriginateUssdNoWaitOutput* o) {
                guint16 errCode = 0;
                QmiVoiceCallEndReason cause;
                GArray* utf16 = nullptr;
                GArray* data = nullptr;
                QmiVoiceUssDataCodingScheme dcs = QMI_VOICE_USS_DATA_CODING_SCHEME_UNKNOWN;
                bool hasErr = qmi_indication_voice_originate_ussd_no_wait_output_get_error_code(
                        o, &errCode, nullptr);
                bool hasCause = qmi_indication_voice_originate_ussd_no_wait_output_get_failure_cause(
                        o, &cause, nullptr);
                std::string msg;
                if (qmi_indication_voice_originate_ussd_no_wait_output_get_uss_data_utf16(o, &utf16,
                                                                                          nullptr))
                    msg = ussUtf16ToUtf8(utf16);
                else if (qmi_indication_voice_originate_ussd_no_wait_output_get_uss_data(
                                 o, &dcs, &data, nullptr))
                    msg = ussDataToUtf8(dcs, data);
                if ((hasErr && errCode) || (hasCause && msg.empty())) {
                    LOG(WARNING) << "USSD originate failed: error " << errCode << " cause "
                                 << (hasCause ? int(cause) : -1);
                    ussdIndication(aidlVoice::UssdModeType::NOT_SUPPORTED, msg);
                    return;
                }
                /* a follow-up "ussd" indication asks for input if the network wants it */
                ussdIndication(aidlVoice::UssdModeType::NOTIFY, msg);
            });
    /* network-initiated / further USSD messages */
    m.onIndication<QmiIndicationVoiceUssdOutput>(
            QMI_CLIENT(m.voice), "ussd", [this](QmiIndicationVoiceUssdOutput* o) {
                QmiVoiceUserAction action = QMI_VOICE_USER_ACTION_UNKNOWN;
                GArray* utf16 = nullptr;
                GArray* data = nullptr;
                QmiVoiceUssDataCodingScheme dcs = QMI_VOICE_USS_DATA_CODING_SCHEME_UNKNOWN;
                qmi_indication_voice_ussd_output_get_user_action(o, &action, nullptr);
                std::string msg;
                if (qmi_indication_voice_ussd_output_get_uss_data_utf16(o, &utf16, nullptr))
                    msg = ussUtf16ToUtf8(utf16);
                else if (qmi_indication_voice_ussd_output_get_uss_data(o, &dcs, &data, nullptr))
                    msg = ussDataToUtf8(dcs, data);
                if (action == QMI_VOICE_USER_ACTION_REQUIRED) {
                    std::lock_guard lk(mUssdLock);
                    mUssdActive = true;
                }
                ussdIndication(action == QMI_VOICE_USER_ACTION_REQUIRED
                                       ? aidlVoice::UssdModeType::REQUEST
                                       : aidlVoice::UssdModeType::NOTIFY,
                               msg);
            });
    /* "release-ussd" carries no output object: connect a 0-argument handler */
    auto* client = QMI_CLIENT(m.voice);
    m.runOnLoop([this, client] {
        g_signal_connect_data(client, "release-ussd",
                              G_CALLBACK(+[](QmiClient*, gpointer ud) {
                                  static_cast<RadioVoice*>(ud)->ussdIndication(
                                          aidlVoice::UssdModeType::NW_RELEASE, "");
                              }),
                              this, nullptr, GConnectFlags(0));
    });
}

Status RadioVoice::sendUssd(int32_t serial, const std::string& ussd) {
    LOG_CALL << ussd.size() << " chars";
    mExec.post([this, serial, ussd] {
        bool answer;
        {
            std::lock_guard lk(mUssdLock);
            answer = mUssdActive && mUssdWaitingForUser;
        }
        g_autoptr(GError) err = nullptr;
        g_autoptr(GArray) data = g_array_new(FALSE, FALSE, 1);
        g_array_append_vals(data, ussd.data(), ussd.size());
        bool okay;
        if (answer) {
            auto* in = qmi_message_voice_answer_ussd_input_new();
            qmi_message_voice_answer_ussd_input_set_uss_data(in, QMI_VOICE_USS_DATA_CODING_SCHEME_ASCII,
                                                             data, nullptr);
            auto out = QMI_SYNC(voice, answer_ussd, QmiModem::get().voice, in, &err);
            qmi_message_voice_answer_ussd_input_unref(in);
            okay = out != nullptr;
        } else {
            auto* in = qmi_message_voice_originate_ussd_no_wait_input_new();
            qmi_message_voice_originate_ussd_no_wait_input_set_uss_data(
                    in, QMI_VOICE_USS_DATA_CODING_SCHEME_ASCII, data, nullptr);
            auto out = QMI_SYNC(voice, originate_ussd_no_wait, QmiModem::get().voice, in, &err);
            qmi_message_voice_originate_ussd_no_wait_input_unref(in);
            okay = out != nullptr;
        }
        if (!okay) {
            LOG(ERROR) << (answer ? "answer_ussd: " : "originate_ussd: ") << err->message;
            respond()->sendUssdResponse(errorResponse(serial, RadioError::GENERIC_FAILURE));
            return;
        }
        {
            std::lock_guard lk(mUssdLock);
            mUssdActive = true;
            mUssdWaitingForUser = false;
        }
        LOG(INFO) << (answer ? "USSD answer sent" : "USSD originated");
        respond()->sendUssdResponse(noError(serial));
    });
    return ok();
}

Status RadioVoice::cancelPendingUssd(int32_t serial) {
    LOG_CALL;
    mExec.post([this, serial] {
        g_autoptr(GError) err = nullptr;
        auto out = QMI_SYNC(voice, cancel_ussd, QmiModem::get().voice, nullptr, &err);
        {
            std::lock_guard lk(mUssdLock);
            mUssdActive = false;
            mUssdWaitingForUser = false;
        }
        if (!out) LOG(WARNING) << "cancel_ussd: " << err->message;
        respond()->cancelPendingUssdResponse(noError(serial));
    });
    return ok();
}

Status RadioVoice::responseAcknowledgement() {
    return ok();
}

Status RadioVoice::setResponseFunctions(
        const std::shared_ptr<aidlVoice::IRadioVoiceResponse>& response,
        const std::shared_ptr<aidlVoice::IRadioVoiceIndication>& indication) {
    LOG_CALL_NOSERIAL;
    respond = response;
    indicate = indication;
    setResponseFunctionsBase();
    return ok();
}

}  // namespace angler::radio
