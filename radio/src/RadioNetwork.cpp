/*
 * Network: NAS service (serving system, signal info, selection).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "qmiril"

#include "RadioNetwork.h"

#include <format>

#include <aidl/android/hardware/radio/RadioConst.h>
#include <libminradio/debug.h>
#include <libminradio/network/structs.h>
#include <libminradio/response.h>

#define RADIO_MODULE "Network"

namespace angler::radio {

using ::aidl::android::hardware::radio::RadioConst;
using ::aidl::android::hardware::radio::RadioError;
using ::aidl::android::hardware::radio::RadioIndicationType;
using ::aidl::android::hardware::radio::RadioTechnology;
using ::android::hardware::radio::minimal::errorResponse;
using ::android::hardware::radio::minimal::noError;
using ::ndk::ScopedAStatus;
namespace aidl = ::aidl::android::hardware::radio::network;
namespace minimal = ::android::hardware::radio::minimal;
constexpr auto ok = &ScopedAStatus::ok;

static RadioTechnology mapRat(QmiNasRadioInterface rat) {
    switch (rat) {
        case QMI_NAS_RADIO_INTERFACE_GSM:
            return RadioTechnology::GSM;
        case QMI_NAS_RADIO_INTERFACE_UMTS:
            return RadioTechnology::UMTS;
        case QMI_NAS_RADIO_INTERFACE_LTE:
            return RadioTechnology::LTE;
        case QMI_NAS_RADIO_INTERFACE_TD_SCDMA:
            return RadioTechnology::TD_SCDMA;
        default:
            return RadioTechnology::UNKNOWN;
    }
}

void RadioNetwork::setupIndications() {
    auto& m = QmiModem::get();
    g_autoptr(GError) err = nullptr;

    auto* in = qmi_message_nas_register_indications_input_new();
    qmi_message_nas_register_indications_input_set_serving_system_events(in, TRUE, nullptr);
    qmi_message_nas_register_indications_input_set_signal_info(in, TRUE, nullptr);
    auto out = QMI_SYNC(nas, register_indications, m.nas, in, &err);
    qmi_message_nas_register_indications_input_unref(in);
    if (!out) LOG(WARNING) << "nas register_indications: " << err->message;

    /* Signal reports on threshold crossings (dBm) */
    g_clear_error(&err);
    auto* cfg = qmi_message_nas_config_signal_info_input_new();
    g_autoptr(GArray) rssi = g_array_new(FALSE, FALSE, sizeof(gint8));
    for (gint8 v : {-110, -103, -97, -91, -85, -79, -73}) g_array_append_val(rssi, v);
    qmi_message_nas_config_signal_info_input_set_rssi_threshold(cfg, rssi, nullptr);
    g_autoptr(GArray) rsrp = g_array_new(FALSE, FALSE, sizeof(gint16));
    for (gint16 v : {-128, -118, -108, -98, -88}) g_array_append_val(rsrp, v);
    qmi_message_nas_config_signal_info_input_set_rsrp_threshold(cfg, rsrp, nullptr);
    auto cfgOut = QMI_SYNC(nas, config_signal_info, m.nas, cfg, &err);
    qmi_message_nas_config_signal_info_input_unref(cfg);
    if (!cfgOut) LOG(WARNING) << "nas config_signal_info: " << err->message;

    m.onIndication<QmiIndicationNasServingSystemOutput>(
            QMI_CLIENT(m.nas), "serving-system", [this](QmiIndicationNasServingSystemOutput*) {
                indicate()->networkStateChanged(RadioIndicationType::UNSOLICITED);
            });
    m.onIndication<QmiIndicationNasSignalInfoOutput>(
            QMI_CLIENT(m.nas), "signal-info", [this](QmiIndicationNasSignalInfoOutput*) {
                mExec.post([this] {
                    indicate()->currentSignalStrength(RadioIndicationType::UNSOLICITED,
                                                      querySignal());
                });
            });
    indicate()->networkStateChanged(RadioIndicationType::UNSOLICITED);
}

RadioNetwork::Serving RadioNetwork::queryServing() {
    Serving s;
    g_autoptr(GError) err = nullptr;
    auto out = QMI_SYNC(nas, get_serving_system, QmiModem::get().nas, nullptr, &err);
    if (!out) {
        LOG(ERROR) << "get_serving_system: " << err->message;
        return s;
    }
    QmiNasNetworkType selected;
    GArray* rats = nullptr;
    if (!qmi_message_nas_get_serving_system_output_get_serving_system(
                out.get(), &s.reg, &s.cs, &s.ps, &selected, &rats, nullptr)) {
        return s;
    }
    s.ok = true;
    if (rats && rats->len > 0) s.rat = g_array_index(rats, QmiNasRadioInterface, 0);

    QmiNasRoamingIndicatorStatus roam;
    if (qmi_message_nas_get_serving_system_output_get_roaming_indicator(out.get(), &roam, nullptr))
        s.roaming = roam == QMI_NAS_ROAMING_INDICATOR_STATUS_ON;

    guint16 mcc = 0, mnc = 0;
    const gchar* desc = nullptr;
    if (qmi_message_nas_get_serving_system_output_get_current_plmn(out.get(), &mcc, &mnc, &desc,
                                                                   nullptr)) {
        gboolean pcs = FALSE;
        guint16 m2, n2;
        qmi_message_nas_get_serving_system_output_get_mnc_pcs_digit_include_status(
                out.get(), &m2, &n2, &pcs, nullptr);
        s.mcc = std::format("{:03}", mcc);
        s.mnc = pcs ? std::format("{:03}", mnc) : std::format("{:02}", mnc);
        s.name = desc ? desc : "";
    }
    guint32 cid = 0;
    guint16 lac = 0, tac = 0;
    if (qmi_message_nas_get_serving_system_output_get_cid_3gpp(out.get(), &cid, nullptr))
        s.cid = cid;
    if (qmi_message_nas_get_serving_system_output_get_lac_3gpp(out.get(), &lac, nullptr))
        s.lac = lac;
    if (qmi_message_nas_get_serving_system_output_get_lte_tac(out.get(), &tac, nullptr))
        s.tac = tac;
    return s;
}

aidl::RegStateResult RadioNetwork::toRegState(const Serving& s, bool data) {
    aidl::RegStateResult r{};
    if (!s.ok) return r;

    switch (s.reg) {
        case QMI_NAS_REGISTRATION_STATE_REGISTERED:
            r.regState = s.roaming ? aidl::RegState::REG_ROAMING : aidl::RegState::REG_HOME;
            break;
        case QMI_NAS_REGISTRATION_STATE_NOT_REGISTERED_SEARCHING:
            r.regState = aidl::RegState::NOT_REG_MT_SEARCHING_OP;
            break;
        case QMI_NAS_REGISTRATION_STATE_REGISTRATION_DENIED:
            r.regState = aidl::RegState::REG_DENIED;
            break;
        case QMI_NAS_REGISTRATION_STATE_NOT_REGISTERED:
            r.regState = aidl::RegState::NOT_REG_MT_NOT_SEARCHING_OP;
            break;
        default:
            r.regState = aidl::RegState::UNKNOWN;
    }
    /* Domain specific: not attached to the PS/CS domain -> not registered there */
    auto attach = data ? s.ps : s.cs;
    if ((r.regState == aidl::RegState::REG_HOME || r.regState == aidl::RegState::REG_ROAMING) &&
        attach == QMI_NAS_ATTACH_STATE_DETACHED) {
        r.regState = aidl::RegState::NOT_REG_MT_SEARCHING_OP;
    }

    r.rat = mapRat(s.rat);
    r.registeredPlmn = s.mcc + s.mnc;
    aidl::OperatorInfo op{
            .alphaLong = s.name,
            .alphaShort = s.name,
            .operatorNumeric = s.mcc + s.mnc,
            .status = aidl::OperatorInfo::STATUS_CURRENT,
    };
    switch (s.rat) {
        case QMI_NAS_RADIO_INTERFACE_LTE:
            r.cellIdentity = aidl::CellIdentity::make<aidl::CellIdentity::lte>(aidl::CellIdentityLte{
                    .mcc = s.mcc,
                    .mnc = s.mnc,
                    .ci = static_cast<int32_t>(s.cid),
                    .pci = RadioConst::VALUE_UNAVAILABLE,
                    .tac = s.tac,
                    .earfcn = RadioConst::VALUE_UNAVAILABLE,
                    .operatorNames = op,
                    .bandwidth = RadioConst::VALUE_UNAVAILABLE,
                    .additionalPlmns = {},
                    .csgInfo = std::nullopt,
                    .bands = {},
            });
            r.accessTechnologySpecificInfo =
                    aidl::AccessTechnologySpecificInfo::make<
                            aidl::AccessTechnologySpecificInfo::eutranInfo>(
                            aidl::EutranRegistrationInfo{});
            break;
        case QMI_NAS_RADIO_INTERFACE_UMTS:
            r.cellIdentity =
                    aidl::CellIdentity::make<aidl::CellIdentity::wcdma>(aidl::CellIdentityWcdma{
                            .mcc = s.mcc,
                            .mnc = s.mnc,
                            .lac = s.lac,
                            .cid = static_cast<int32_t>(s.cid),
                            .psc = RadioConst::VALUE_UNAVAILABLE,
                            .uarfcn = RadioConst::VALUE_UNAVAILABLE,
                            .operatorNames = op,
                            .additionalPlmns = {},
                            .csgInfo = std::nullopt,
                    });
            break;
        case QMI_NAS_RADIO_INTERFACE_GSM:
            r.cellIdentity = aidl::CellIdentity::make<aidl::CellIdentity::gsm>(aidl::CellIdentityGsm{
                    .mcc = s.mcc,
                    .mnc = s.mnc,
                    .lac = s.lac,
                    .cid = static_cast<int32_t>(s.cid),
                    .arfcn = RadioConst::VALUE_UNAVAILABLE,
                    .bsic = static_cast<int8_t>(0xFF),
                    .operatorNames = op,
                    .additionalPlmns = {},
            });
            break;
        default:
            break;
    }
    return r;
}

ScopedAStatus RadioNetwork::getVoiceRegistrationState(int32_t serial) {
    LOG_CALL;
    mExec.post([this, serial] {
        if (!QmiModem::get().isReady()) {
            respond()->getVoiceRegistrationStateResponse(
                    errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE), {});
            return;
        }
        respond()->getVoiceRegistrationStateResponse(noError(serial),
                                                     toRegState(queryServing(), false));
    });
    return ok();
}

ScopedAStatus RadioNetwork::getDataRegistrationState(int32_t serial) {
    LOG_CALL;
    mExec.post([this, serial] {
        if (!QmiModem::get().isReady()) {
            respond()->getDataRegistrationStateResponse(
                    errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE), {});
            return;
        }
        respond()->getDataRegistrationStateResponse(noError(serial),
                                                    toRegState(queryServing(), true));
    });
    return ok();
}

ScopedAStatus RadioNetwork::getVoiceRadioTechnology(int32_t serial) {
    LOG_CALL;
    mExec.post([this, serial] {
        auto s = queryServing();
        respond()->getVoiceRadioTechnologyResponse(noError(serial), mapRat(s.rat));
    });
    return ok();
}

aidl::SignalStrength RadioNetwork::querySignal() {
    auto sig = minimal::structs::makeSignalStrength();
    g_autoptr(GError) err = nullptr;
    auto out = QMI_SYNC(nas, get_signal_info, QmiModem::get().nas, nullptr, &err);
    if (!out) {
        LOG(WARNING) << "get_signal_info: " << err->message;
        return sig;
    }
    gint8 rssi;
    gint8 rsrq;
    gint16 rsrp, snr, ecio;
    if (qmi_message_nas_get_signal_info_output_get_lte_signal_strength(out.get(), &rssi, &rsrq,
                                                                       &rsrp, &snr, nullptr)) {
        sig.lte.signalStrength = minimal::structs::rssiToSignalStrength(rssi);
        sig.lte.rsrp = minimal::structs::validateRsrp(rsrp);   /* helpers take dBm (negative) */
        sig.lte.rsrq = minimal::structs::validateRsrq(rsrq);
        sig.lte.rssnr = snr; /* both in 0.1 dB */
    }
    if (qmi_message_nas_get_signal_info_output_get_wcdma_signal_strength(out.get(), &rssi, &ecio,
                                                                         nullptr)) {
        sig.wcdma.signalStrength = minimal::structs::rssiToSignalStrength(rssi);
        sig.wcdma.bitErrorRate = 99;
    }
    if (qmi_message_nas_get_signal_info_output_get_gsm_signal_strength(out.get(), &rssi,
                                                                       nullptr)) {
        sig.gsm.signalStrength = minimal::structs::rssiToSignalStrength(rssi);
        sig.gsm.bitErrorRate = 99;
    }
    return sig;
}

ScopedAStatus RadioNetwork::getSignalStrength(int32_t serial) {
    LOG_CALL;
    mExec.post([this, serial] { respond()->getSignalStrengthResponse(noError(serial), querySignal()); });
    return ok();
}

ScopedAStatus RadioNetwork::getNetworkSelectionMode(int32_t serial) {
    LOG_CALL;
    respond()->getNetworkSelectionModeResponse(noError(serial), /*manual*/ false);
    return ok();
}

ScopedAStatus RadioNetwork::setNetworkSelectionModeAutomatic(int32_t serial) {
    LOG_CALL;
    respond()->setNetworkSelectionModeAutomaticResponse(noError(serial));
    return ok();
}

}  // namespace angler::radio
