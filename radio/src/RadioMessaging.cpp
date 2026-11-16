/*
 * Messaging: WMS service, GSM/UMTS SMS only.
 *
 * MT SMS are routed "transfer only" to the AP and acknowledged by Android
 * (acknowledgeLastIncomingGsmSms -> WMS Send Ack). Messages the modem stored
 * anyway are read, delivered and deleted.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "qmiril"

#include "RadioMessaging.h"

#include <libminradio/debug.h>
#include <libminradio/response.h>

#define RADIO_MODULE "Messaging"

namespace angler::radio {

using ::aidl::android::hardware::radio::RadioError;
using ::aidl::android::hardware::radio::RadioIndicationType;
using ::android::hardware::radio::minimal::errorResponse;
using ::android::hardware::radio::minimal::noError;
using ::android::hardware::radio::minimal::notSupported;
using Status = ::ndk::ScopedAStatus;
constexpr auto ok = &Status::ok;

static std::vector<uint8_t> toVector(GArray* a) {
    if (!a) return {};
    auto* p = reinterpret_cast<const uint8_t*>(a->data);
    return {p, p + a->len};
}

static std::string hex(GArray* a) {
    static const char* d = "0123456789abcdef";
    std::string out;
    if (!a) return out;
    auto* p = reinterpret_cast<const uint8_t*>(a->data);
    for (guint i = 0; i < a->len; i++) {
        out += d[p[i] >> 4];
        out += d[p[i] & 15];
    }
    return out;
}

void RadioMessaging::setupIndications() {
    auto& m = QmiModem::get();
    if (!m.wms) {
        LOG(ERROR) << "no WMS client, SMS unavailable";
        return;
    }
    g_autoptr(GError) err = nullptr;

    /* Hand every point-to-point class to the AP without storing it */
    auto* routes = qmi_message_wms_set_routes_input_new();
    g_autoptr(GArray) list =
            g_array_new(FALSE, FALSE, sizeof(QmiMessageWmsSetRoutesInputRouteListElement));
    for (QmiWmsMessageClass cls : {QMI_WMS_MESSAGE_CLASS_0, QMI_WMS_MESSAGE_CLASS_1,
                                   QMI_WMS_MESSAGE_CLASS_2, QMI_WMS_MESSAGE_CLASS_3,
                                   QMI_WMS_MESSAGE_CLASS_NONE}) {
        QmiMessageWmsSetRoutesInputRouteListElement e{
                .message_type = QMI_WMS_MESSAGE_TYPE_POINT_TO_POINT,
                .message_class = cls,
                .storage = QMI_WMS_STORAGE_TYPE_NONE,
                .receipt_action = QMI_WMS_RECEIPT_ACTION_TRANSFER_ONLY,
        };
        g_array_append_val(list, e);
    }
    qmi_message_wms_set_routes_input_set_route_list(routes, list, nullptr);
    auto rOut = QMI_SYNC(wms, set_routes, m.wms, routes, &err);
    qmi_message_wms_set_routes_input_unref(routes);
    if (!rOut) LOG(WARNING) << "wms set_routes: " << err->message;

    g_clear_error(&err);
    auto* ev = qmi_message_wms_set_event_report_input_new();
    qmi_message_wms_set_event_report_input_set_new_mt_message_indicator(ev, TRUE, nullptr);
    auto eOut = QMI_SYNC(wms, set_event_report, m.wms, ev, &err);
    qmi_message_wms_set_event_report_input_unref(ev);
    if (!eOut) LOG(WARNING) << "wms set_event_report: " << err->message;

    m.onIndication<QmiIndicationWmsEventReportOutput>(
            QMI_CLIENT(m.wms), "event-report", [this](QmiIndicationWmsEventReportOutput* out) {
                QmiWmsAckIndicator ackInd;
                guint32 transaction;
                QmiWmsMessageFormat format;
                GArray* raw = nullptr;
                if (qmi_indication_wms_event_report_output_get_transfer_route_mt_message(
                            out, &ackInd, &transaction, &format, &raw, nullptr)) {
                    if (format != QMI_WMS_MESSAGE_FORMAT_GSM_WCDMA_POINT_TO_POINT) {
                        LOG(WARNING) << "ignoring MT message format " << format;
                        return;
                    }
                    LOG(INFO) << "MT SMS, " << raw->len << " bytes, transaction " << transaction
                              << ", ack " << ackInd;
                    LOG(DEBUG) << "MT SMS pdu " << hex(raw);
                    {
                        std::lock_guard lk(mAckLock);
                        mAckPending = ackInd == QMI_WMS_ACK_INDICATOR_SEND;
                        mAckTransaction = transaction;
                    }
                    /* transfer-route data is the bare TPDU; Android wants
                     * SMSC address first -> prepend an empty one (length 0) */
                    auto pdu = toVector(raw);
                    pdu.insert(pdu.begin(), 0x00);
                    indicate()->newSms(RadioIndicationType::UNSOLICITED_ACK_EXP, pdu);
                    return;
                }
                QmiWmsStorageType storage;
                guint32 index;
                if (qmi_indication_wms_event_report_output_get_mt_message(out, &storage, &index,
                                                                          nullptr)) {
                    mExec.post([this, storage, index] { readStored(storage, index); });
                }
            });
}

void RadioMessaging::readStored(QmiWmsStorageType storage, guint32 index) {
    auto& m = QmiModem::get();
    g_autoptr(GError) err = nullptr;
    auto* in = qmi_message_wms_raw_read_input_new();
    qmi_message_wms_raw_read_input_set_message_memory_storage_id(in, storage, index, nullptr);
    qmi_message_wms_raw_read_input_set_message_mode(in, QMI_WMS_MESSAGE_MODE_GSM_WCDMA, nullptr);
    auto out = QMI_SYNC(wms, raw_read, m.wms, in, &err);
    qmi_message_wms_raw_read_input_unref(in);
    if (!out) {
        LOG(ERROR) << "wms raw_read " << index << ": " << err->message;
        return;
    }
    QmiWmsMessageTagType tag;
    QmiWmsMessageFormat format;
    GArray* raw = nullptr;
    qmi_message_wms_raw_read_output_get_raw_message_data(out.get(), &tag, &format, &raw, nullptr);
    {
        std::lock_guard lk(mAckLock);
        mAckPending = false; /* stored by the modem: nothing to ack on the network side */
    }
    if (raw) LOG(DEBUG) << "stored SMS " << index << " format " << format << " pdu " << hex(raw);
    if (raw) indicate()->newSms(RadioIndicationType::UNSOLICITED_ACK_EXP, toVector(raw));

    g_clear_error(&err);
    auto* del = qmi_message_wms_delete_input_new();
    qmi_message_wms_delete_input_set_memory_storage(del, storage, nullptr);
    qmi_message_wms_delete_input_set_memory_index(del, index, nullptr);
    qmi_message_wms_delete_input_set_message_mode(del, QMI_WMS_MESSAGE_MODE_GSM_WCDMA, nullptr);
    auto dOut = QMI_SYNC(wms, delete, m.wms, del, &err);
    qmi_message_wms_delete_input_unref(del);
    if (!dOut) LOG(WARNING) << "wms delete " << index << ": " << err->message;
}

void RadioMessaging::ack(bool success) {
    guint32 transaction;
    {
        std::lock_guard lk(mAckLock);
        if (!mAckPending) return;
        mAckPending = false;
        transaction = mAckTransaction;
    }
    g_autoptr(GError) err = nullptr;
    auto* in = qmi_message_wms_send_ack_input_new();
    qmi_message_wms_send_ack_input_set_information(in, transaction,
                                                   QMI_WMS_MESSAGE_PROTOCOL_WCDMA, success,
                                                   nullptr);
    auto out = QMI_SYNC(wms, send_ack, QmiModem::get().wms, in, &err);
    qmi_message_wms_send_ack_input_unref(in);
    if (!out) LOG(WARNING) << "wms send_ack: " << err->message;
}

void RadioMessaging::doSend(int32_t serial, const aidlMsg::GsmSmsMessage& message,
                            bool expectMore) {
    auto respondFn = [this, expectMore](const auto& info, const aidlMsg::SendSmsResult& r) {
        if (expectMore)
            respond()->sendSmsExpectMoreResponse(info, r);
        else
            respond()->sendSmsResponse(info, r);
    };
    /* QMI wants the full 27.005 PDU: SMSC address (0x00 = default) + TPDU */
    std::string hex = message.smscPdu.empty() ? "00" : message.smscPdu;
    hex += message.pdu;
    g_autoptr(GArray) raw = hexToGArray(hex);

    auto* in = qmi_message_wms_raw_send_input_new();
    qmi_message_wms_raw_send_input_set_raw_message_data(
            in, QMI_WMS_MESSAGE_FORMAT_GSM_WCDMA_POINT_TO_POINT, raw, nullptr);
    g_autoptr(GError) err = nullptr;
    auto out = qmiSync(QmiModem::get().wms, qmi_client_wms_raw_send,
                       qmi_client_wms_raw_send_finish, qmi_message_wms_raw_send_output_get_result,
                       qmi_message_wms_raw_send_output_unref, in, &err, 60);
    qmi_message_wms_raw_send_input_unref(in);
    if (!out) {
        LOG(ERROR) << "wms raw_send: " << err->message;
        respondFn(errorResponse(serial, RadioError::SMS_SEND_FAIL_RETRY),
                  {.messageRef = 0, .ackPDU = "", .errorCode = -1});
        return;
    }
    guint16 ref = 0;
    qmi_message_wms_raw_send_output_get_message_id(out.get(), &ref, nullptr);
    LOG(INFO) << "SMS sent, message id " << ref;
    respondFn(noError(serial), {.messageRef = ref, .ackPDU = "", .errorCode = -1});
}

Status RadioMessaging::sendSms(int32_t serial, const aidlMsg::GsmSmsMessage& message) {
    LOG_CALL;
    mExec.post([this, serial, message] { doSend(serial, message, false); });
    return ok();
}

Status RadioMessaging::sendSmsExpectMore(int32_t serial, const aidlMsg::GsmSmsMessage& message) {
    LOG_CALL;
    mExec.post([this, serial, message] { doSend(serial, message, true); });
    return ok();
}

Status RadioMessaging::acknowledgeLastIncomingGsmSms(int32_t serial, bool success,
                                                     aidlMsg::SmsAcknowledgeFailCause cause) {
    LOG_CALL << success << " " << static_cast<int>(cause);
    mExec.post([this, serial, success] {
        ack(success);
        respond()->acknowledgeLastIncomingGsmSmsResponse(noError(serial));
    });
    return ok();
}

Status RadioMessaging::acknowledgeIncomingGsmSmsWithPdu(int32_t serial, bool success,
                                                        const std::string&) {
    LOG_CALL << success;
    mExec.post([this, serial, success] {
        ack(success);
        respond()->acknowledgeIncomingGsmSmsWithPduResponse(noError(serial));
    });
    return ok();
}

Status RadioMessaging::reportSmsMemoryStatus(int32_t serial, bool available) {
    LOG_CALL << available;
    respond()->reportSmsMemoryStatusResponse(noError(serial));
    return ok();
}

Status RadioMessaging::getSmscAddress(int32_t serial) {
    LOG_CALL;
    respond()->getSmscAddressResponse(notSupported(serial), "");
    return ok();
}

Status RadioMessaging::setSmscAddress(int32_t serial, const std::string&) {
    LOG_CALL;
    respond()->setSmscAddressResponse(notSupported(serial));
    return ok();
}

Status RadioMessaging::getGsmBroadcastConfig(int32_t serial) {
    LOG_CALL;
    respond()->getGsmBroadcastConfigResponse(noError(serial), {});
    return ok();
}

Status RadioMessaging::setGsmBroadcastConfig(
        int32_t serial, const std::vector<aidlMsg::GsmBroadcastSmsConfigInfo>&) {
    LOG_CALL;
    respond()->setGsmBroadcastConfigResponse(noError(serial));
    return ok();
}

Status RadioMessaging::setGsmBroadcastActivation(int32_t serial, bool activate) {
    LOG_CALL << activate;
    respond()->setGsmBroadcastActivationResponse(noError(serial));
    return ok();
}

Status RadioMessaging::deleteSmsOnSim(int32_t serial, int32_t index) {
    LOG_CALL << index;
    respond()->deleteSmsOnSimResponse(notSupported(serial));
    return ok();
}

Status RadioMessaging::writeSmsToSim(int32_t serial, const aidlMsg::SmsWriteArgs&) {
    LOG_CALL;
    respond()->writeSmsToSimResponse(notSupported(serial), -1);
    return ok();
}

/* CDMA / IMS: not applicable */
Status RadioMessaging::acknowledgeLastIncomingCdmaSms(int32_t serial, const aidlMsg::CdmaSmsAck&) {
    respond()->acknowledgeLastIncomingCdmaSmsResponse(notSupported(serial));
    return ok();
}
Status RadioMessaging::deleteSmsOnRuim(int32_t serial, int32_t) {
    respond()->deleteSmsOnRuimResponse(notSupported(serial));
    return ok();
}
Status RadioMessaging::getCdmaBroadcastConfig(int32_t serial) {
    respond()->getCdmaBroadcastConfigResponse(notSupported(serial), {});
    return ok();
}
Status RadioMessaging::sendCdmaSms(int32_t serial, const aidlMsg::CdmaSmsMessage&) {
    respond()->sendCdmaSmsResponse(notSupported(serial), {});
    return ok();
}
Status RadioMessaging::sendCdmaSmsExpectMore(int32_t serial, const aidlMsg::CdmaSmsMessage&) {
    respond()->sendCdmaSmsExpectMoreResponse(notSupported(serial), {});
    return ok();
}
Status RadioMessaging::sendImsSms(int32_t serial, const aidlMsg::ImsSmsMessage&) {
    respond()->sendImsSmsResponse(notSupported(serial), {});
    return ok();
}
Status RadioMessaging::setCdmaBroadcastActivation(int32_t serial, bool) {
    respond()->setCdmaBroadcastActivationResponse(notSupported(serial));
    return ok();
}
Status RadioMessaging::setCdmaBroadcastConfig(
        int32_t serial, const std::vector<aidlMsg::CdmaBroadcastSmsConfigInfo>&) {
    respond()->setCdmaBroadcastConfigResponse(notSupported(serial));
    return ok();
}
Status RadioMessaging::writeSmsToRuim(int32_t serial, const aidlMsg::CdmaSmsWriteArgs&) {
    respond()->writeSmsToRuimResponse(notSupported(serial), -1);
    return ok();
}

Status RadioMessaging::responseAcknowledgement() {
    return ok();
}

Status RadioMessaging::setResponseFunctions(
        const std::shared_ptr<aidlMsg::IRadioMessagingResponse>& response,
        const std::shared_ptr<aidlMsg::IRadioMessagingIndication>& indication) {
    LOG_CALL_NOSERIAL;
    respond = response;
    indicate = indication;
    setResponseFunctionsBase();
    return ok();
}

}  // namespace angler::radio
