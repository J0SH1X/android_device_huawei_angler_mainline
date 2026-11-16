/*
 * Modem: DMS service (identity, revision, operating mode).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "qmiril"

#include "RadioModem.h"

#include <atomic>

#include <libminradio/debug.h>
#include <libminradio/response.h>

#define RADIO_MODULE "Modem"

namespace angler::radio {

using ::aidl::android::hardware::radio::RadioError;
using ::aidl::android::hardware::radio::RadioIndicationType;
using ::aidl::android::hardware::radio::RadioTechnology;
using ::android::hardware::radio::minimal::errorResponse;
using ::android::hardware::radio::minimal::noError;
using ::ndk::ScopedAStatus;
namespace aidl = ::aidl::android::hardware::radio::modem;
constexpr auto ok = &ScopedAStatus::ok;

RadioModem::RadioModem(std::shared_ptr<::android::hardware::radio::minimal::SlotContext> context)
    : ::android::hardware::radio::minimal::RadioModem(
              context, {{RadioTechnology::GSM, RadioTechnology::GPRS, RadioTechnology::EDGE,
                         RadioTechnology::UMTS, RadioTechnology::HSDPA, RadioTechnology::HSUPA,
                         RadioTechnology::HSPA, RadioTechnology::HSPAP, RadioTechnology::LTE}}) {}

/*
 * Last radio state this HAL set (or -1 before the first SET_OPERATING_MODE).
 * The modem keeps answering the previous mode for a moment after a set, so
 * querying it right after (e.g. onUpdatedResponseFunctions queued behind a
 * setRadioPower) produced stale indications that contradicted the request.
 */
static std::atomic<int> gRadioState{-1};

aidl::RadioState RadioModem::currentRadioState() {
    if (int st = gRadioState.load(); st >= 0) return static_cast<aidl::RadioState>(st);
    g_autoptr(GError) err = nullptr;
    auto out = QMI_SYNC(dms, get_operating_mode, QmiModem::get().dms, nullptr, &err);
    QmiDmsOperatingMode mode = QMI_DMS_OPERATING_MODE_UNKNOWN;
    if (!out || !qmi_message_dms_get_operating_mode_output_get_mode(out.get(), &mode, nullptr)) {
        LOG(ERROR) << "get_operating_mode: " << (err ? err->message : "no mode");
        return aidl::RadioState::UNAVAILABLE;
    }
    return mode == QMI_DMS_OPERATING_MODE_ONLINE ? aidl::RadioState::ON : aidl::RadioState::OFF;
}

void RadioModem::onUpdatedResponseFunctions() {
    mExec.post([this] {
        indicate()->rilConnected(RadioIndicationType::UNSOLICITED);
        indicate()->radioStateChanged(RadioIndicationType::UNSOLICITED, currentRadioState());
    });
}

ScopedAStatus RadioModem::getImei(int32_t serial) {
    LOG_CALL;
    mExec.post([this, serial] {
        g_autoptr(GError) err = nullptr;
        auto out = QMI_SYNC(dms, get_ids, QmiModem::get().dms, nullptr, &err);
        const gchar *imei = nullptr, *svn = nullptr;
        if (!out || !qmi_message_dms_get_ids_output_get_imei(out.get(), &imei, nullptr)) {
            LOG(ERROR) << "get_ids: " << (err ? err->message : "no IMEI");
            respond()->getImeiResponse(errorResponse(serial, RadioError::INTERNAL_ERR),
                                       std::nullopt);
            return;
        }
        qmi_message_dms_get_ids_output_get_imei_software_version(out.get(), &svn, nullptr);
        aidl::ImeiInfo info{
                .type = aidl::ImeiInfo::ImeiType::PRIMARY,
                .imei = imei ? imei : "",
                .svn = svn ? svn : "00",
        };
        respond()->getImeiResponse(noError(serial), info);
    });
    return ok();
}

ScopedAStatus RadioModem::getBasebandVersion(int32_t serial) {
    LOG_CALL;
    mExec.post([this, serial] {
        g_autoptr(GError) err = nullptr;
        auto out = QMI_SYNC(dms, get_revision, QmiModem::get().dms, nullptr, &err);
        const gchar* rev = nullptr;
        if (!out || !qmi_message_dms_get_revision_output_get_revision(out.get(), &rev, nullptr)) {
            respond()->getBasebandVersionResponse(errorResponse(serial, RadioError::INTERNAL_ERR),
                                                  "");
            return;
        }
        respond()->getBasebandVersionResponse(noError(serial), rev ? rev : "");
    });
    return ok();
}

ScopedAStatus RadioModem::setRadioPower(int32_t serial, bool powerOn, bool forEmergencyCall,
                                        bool preferredForEmergencyCall) {
    LOG_CALL << powerOn << " " << forEmergencyCall << " " << preferredForEmergencyCall;
    mExec.post([this, serial, powerOn] {
        auto* in = qmi_message_dms_set_operating_mode_input_new();
        qmi_message_dms_set_operating_mode_input_set_mode(
                in, powerOn ? QMI_DMS_OPERATING_MODE_ONLINE : QMI_DMS_OPERATING_MODE_LOW_POWER,
                nullptr);
        g_autoptr(GError) err = nullptr;
        auto out = QMI_SYNC(dms, set_operating_mode, QmiModem::get().dms, in, &err);
        qmi_message_dms_set_operating_mode_input_unref(in);
        if (!out) {
            LOG(ERROR) << "set_operating_mode(" << powerOn << "): " << err->message;
            /* "no effect" means it is already in that mode */
            if (!g_error_matches(err, QMI_PROTOCOL_ERROR, QMI_PROTOCOL_ERROR_NO_EFFECT)) {
                respond()->setRadioPowerResponse(errorResponse(serial, RadioError::INTERNAL_ERR));
                return;
            }
        }
        gRadioState = static_cast<int>(powerOn ? aidl::RadioState::ON : aidl::RadioState::OFF);
        respond()->setRadioPowerResponse(noError(serial));
        /*
         * Report what was requested: right after SET_OPERATING_MODE the
         * modem still answers the old mode for a moment, and reporting
         * ON after a LOW_POWER request left the framework believing the
         * radio was on while the modem sat in low-power (never searched).
         */
        indicate()->radioStateChanged(RadioIndicationType::UNSOLICITED,
                                      powerOn ? aidl::RadioState::ON : aidl::RadioState::OFF);
    });
    return ok();
}

}  // namespace angler::radio
