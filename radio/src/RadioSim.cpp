/*
 * SIM: UIM service (card status, PIN, file access) + DMS (IMSI, ICCID).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "qmiril"

#include "RadioSim.h"

#include <strings.h>

#include <ios>
#include <string>

#include <libminradio/debug.h>
#include <libminradio/response.h>

#include "State.h"

#define RADIO_MODULE "Sim"

namespace angler::radio {

using ::aidl::android::hardware::radio::RadioError;
using ::aidl::android::hardware::radio::RadioIndicationType;
using ::android::hardware::radio::minimal::errorResponse;
using ::android::hardware::radio::minimal::noError;
using ::ndk::ScopedAStatus;
namespace aidl = ::aidl::android::hardware::radio::sim;
namespace aidlConfig = ::aidl::android::hardware::radio::config;
constexpr auto ok = &ScopedAStatus::ok;

static aidl::PinState mapPin(QmiUimPinState s) {
    switch (s) {
        case QMI_UIM_PIN_STATE_ENABLED_NOT_VERIFIED:
            return aidl::PinState::ENABLED_NOT_VERIFIED;
        case QMI_UIM_PIN_STATE_ENABLED_VERIFIED:
            return aidl::PinState::ENABLED_VERIFIED;
        case QMI_UIM_PIN_STATE_DISABLED:
            return aidl::PinState::DISABLED;
        case QMI_UIM_PIN_STATE_BLOCKED:
            return aidl::PinState::ENABLED_BLOCKED;
        case QMI_UIM_PIN_STATE_PERMANENTLY_BLOCKED:
            return aidl::PinState::ENABLED_PERM_BLOCKED;
        default:
            return aidl::PinState::UNKNOWN;
    }
}

static int mapAppType(QmiUimCardApplicationType t) {
    switch (t) {
        case QMI_UIM_CARD_APPLICATION_TYPE_SIM:
            return aidl::AppStatus::APP_TYPE_SIM;
        case QMI_UIM_CARD_APPLICATION_TYPE_USIM:
            return aidl::AppStatus::APP_TYPE_USIM;
        case QMI_UIM_CARD_APPLICATION_TYPE_RUIM:
            return aidl::AppStatus::APP_TYPE_RUIM;
        case QMI_UIM_CARD_APPLICATION_TYPE_CSIM:
            return aidl::AppStatus::APP_TYPE_CSIM;
        case QMI_UIM_CARD_APPLICATION_TYPE_ISIM:
            return aidl::AppStatus::APP_TYPE_ISIM;
        default:
            return aidl::AppStatus::APP_TYPE_UNKNOWN;
    }
}

static int mapAppState(QmiUimCardApplicationState s) {
    switch (s) {
        case QMI_UIM_CARD_APPLICATION_STATE_DETECTED:
            return aidl::AppStatus::APP_STATE_DETECTED;
        case QMI_UIM_CARD_APPLICATION_STATE_PIN1_OR_UPIN_PIN_REQUIRED:
            return aidl::AppStatus::APP_STATE_PIN;
        case QMI_UIM_CARD_APPLICATION_STATE_PUK1_OR_UPIN_PUK_REQUIRED:
        case QMI_UIM_CARD_APPLICATION_STATE_PIN1_BLOCKED:
            return aidl::AppStatus::APP_STATE_PUK;
        case QMI_UIM_CARD_APPLICATION_STATE_CHECK_PERSONALIZATION_STATE:
            return aidl::AppStatus::APP_STATE_SUBSCRIPTION_PERSO;
        case QMI_UIM_CARD_APPLICATION_STATE_READY:
            return aidl::AppStatus::APP_STATE_READY;
        default:
            return aidl::AppStatus::APP_STATE_UNKNOWN;
    }
}

/* Message and indication carry identically laid out (but distinct) structs. */
/*
 * This modem rejects DMS UIM_GET_ICCID / UIM_GET_IMSI (InvalidQmiCommand), so
 * read the elementary files through UIM instead.
 */
static std::string uimReadTransparent(QmiUimSessionType st, guint16 fileId, const char* path) {
    auto& m = QmiModem::get();
    g_autoptr(GArray) aid = g_array_new(FALSE, FALSE, 1);
    g_autoptr(GArray) p = simPathToGArray(path);
    g_autoptr(GError) err = nullptr;
    auto* in = qmi_message_uim_read_transparent_input_new();
    qmi_message_uim_read_transparent_input_set_session(in, st, aid, nullptr);
    qmi_message_uim_read_transparent_input_set_file(in, fileId, p, nullptr);
    qmi_message_uim_read_transparent_input_set_read_information(in, 0, 0, nullptr);
    auto out = QMI_SYNC(uim, read_transparent, m.uim, in, &err);
    qmi_message_uim_read_transparent_input_unref(in);
    if (!out) {
        LOG(WARNING) << "uim read " << std::hex << fileId << ": " << err->message;
        return "";
    }
    guint8 sw1 = 0, sw2 = 0;
    GArray* data = nullptr;
    qmi_message_uim_read_transparent_output_get_card_result(out.get(), &sw1, &sw2, nullptr);
    qmi_message_uim_read_transparent_output_get_read_result(out.get(), &data, nullptr);
    if (sw1 != 0x90 && sw1 != 0x91) {
        LOG(WARNING) << "uim read " << std::hex << fileId << ": sw " << int(sw1) << ' ' << int(sw2);
        return "";
    }
    return garrayToHex(data);
}

/* Swapped-nibble BCD (EF_ICCID) -> digits, 'F' filler stripped */
static std::string bcdSwapped(const std::string& hex, size_t startByte = 0) {
    std::string d;
    for (size_t i = startByte * 2; i + 1 < hex.size(); i += 2) {
        d += hex[i + 1];
        d += hex[i];
    }
    while (!d.empty() && (d.back() == 'F' || d.back() == 'f')) d.pop_back();
    return d;
}

static std::string readIccid() {
    /* EF_ICCID 3F00/2FE2, card level, always readable (no PIN) */
    return bcdSwapped(uimReadTransparent(QMI_UIM_SESSION_TYPE_CARD_SLOT_1, 0x2FE2, "3F00"));
}

static std::string readImsi() {
    /* EF_IMSI (6F07) of the GW provisioning app: len, then parity nibble + digits */
    std::string hex = uimReadTransparent(QMI_UIM_SESSION_TYPE_PRIMARY_GW_PROVISIONING, 0x6F07,
                                         "3F007FFF");
    if (hex.size() < 4) return "";
    std::string d(1, hex[2]); /* high nibble of byte 1 = first digit */
    d += bcdSwapped(hex, 2);
    while (!d.empty() && (d.back() == 'F' || d.back() == 'f')) d.pop_back();
    return d;
}

template <typename CardElement, typename AppElement>
static SimSnapshot buildSnapshot(guint16 gwPrimary, GArray* cards) {
    SimSnapshot snap;
    snap.valid = true;
    auto& cs = snap.card;
    cs.cardState = aidl::CardStatus::STATE_ABSENT;
    cs.gsmUmtsSubscriptionAppIndex = -1;
    cs.cdmaSubscriptionAppIndex = -1;
    cs.imsSubscriptionAppIndex = -1;
    cs.slotMap = {.physicalSlotId = 0, .portId = 0};
    cs.supportedMepMode = aidlConfig::MultipleEnabledProfilesMode::NONE;
    if (!cards || cards->len == 0) return snap;

    auto& card = g_array_index(cards, CardElement, 0);
    switch (card.card_state) {
        case QMI_UIM_CARD_STATE_PRESENT:
            cs.cardState = aidl::CardStatus::STATE_PRESENT;
            break;
        case QMI_UIM_CARD_STATE_ERROR:
            cs.cardState = aidl::CardStatus::STATE_ERROR;
            break;
        default:
            cs.cardState = aidl::CardStatus::STATE_ABSENT;
    }
    cs.universalPinState = mapPin(card.upin_state);

    int gwIndex = (gwPrimary == 0xFFFF) ? -1 : (gwPrimary & 0xFF);
    for (guint i = 0; card.applications && i < card.applications->len; i++) {
        auto& app = g_array_index(card.applications, AppElement, i);
        aidl::AppStatus as{};
        as.appType = mapAppType(app.type);
        as.appState = mapAppState(app.state);
        as.persoSubstate = as.appState == aidl::AppStatus::APP_STATE_READY
                                   ? aidl::PersoSubstate::READY
                                   : aidl::PersoSubstate::UNKNOWN;
        as.aidPtr = garrayToHex(app.application_identifier_value);
        as.appLabelPtr = "";
        as.pin1Replaced = app.upin_replaces_pin1;
        as.pin1 = mapPin(app.pin1_state);
        as.pin2 = mapPin(app.pin2_state);
        if (as.appType == aidl::AppStatus::APP_TYPE_ISIM) cs.imsSubscriptionAppIndex = i;
        if (static_cast<int>(i) == gwIndex) {
            snap.gwAidHex = as.aidPtr;
            snap.pin1Retries = app.pin1_retries;
            snap.puk1Retries = app.puk1_retries;
        }
        cs.applications.push_back(std::move(as));
    }
    /* Fall back to the first USIM/SIM app if the modem did not pick one yet */
    if (gwIndex < 0) {
        for (size_t i = 0; i < cs.applications.size(); i++) {
            int t = cs.applications[i].appType;
            if (t == aidl::AppStatus::APP_TYPE_USIM || t == aidl::AppStatus::APP_TYPE_SIM) {
                gwIndex = i;
                snap.gwAidHex = cs.applications[i].aidPtr;
                break;
            }
        }
    }
    cs.gsmUmtsSubscriptionAppIndex = gwIndex;
    return snap;
}

RadioSim::RadioSim(std::shared_ptr<::android::hardware::radio::minimal::SlotContext> context)
    : ::android::hardware::radio::minimal::RadioSim(context) {}

void RadioSim::onUpdatedResponseFunctions() {
    mExec.post([this] { refreshCardStatus(true); });
}

void RadioSim::refreshCardStatus(bool notify) {
    auto& m = QmiModem::get();
    g_autoptr(GError) err = nullptr;
    auto out = QMI_SYNC(uim, get_card_status, m.uim, nullptr, &err);
    if (!out) {
        LOG(ERROR) << "get_card_status: " << err->message;
        return;
    }
    guint16 gw1 = 0xFFFF, x1 = 0xFFFF, gw2 = 0xFFFF, x2 = 0xFFFF;
    GArray* cards = nullptr;
    qmi_message_uim_get_card_status_output_get_card_status(out.get(), &gw1, &x1, &gw2, &x2,
                                                            &cards, nullptr);
    auto snap = buildSnapshot<QmiMessageUimGetCardStatusOutputCardStatusCardsElement,
                              QmiMessageUimGetCardStatusOutputCardStatusCardsElementApplicationsElementV2>(
            gw1, cards);

    if (snap.card.cardState == aidl::CardStatus::STATE_PRESENT && State::get().sim().card.iccid.empty()) {
        std::string iccid = readIccid();
        if (iccid.empty()) {
            g_autoptr(GError) e2 = nullptr;
            auto idOut = QMI_SYNC(dms, uim_get_iccid, m.dms, nullptr, &e2);
            const gchar* id = nullptr;
            if (idOut && qmi_message_dms_uim_get_iccid_output_get_iccid(idOut.get(), &id, nullptr) &&
                id)
                iccid = id;
        }
        if (!iccid.empty()) {
            LOG(INFO) << "ICCID read (" << iccid.size() << " digits)";
            snap.card.iccid = iccid;
            setIccid(iccid);
        } else {
            LOG(WARNING) << "ICCID not readable";
        }
    }
    State::get().setSim(snap);
    LOG(INFO) << "card state " << snap.card.cardState << ", " << snap.card.applications.size()
              << " apps, gw index " << snap.card.gsmUmtsSubscriptionAppIndex;
    if (notify) {
        indicate()->simStatusChanged(RadioIndicationType::UNSOLICITED);
        State::get().notifySimChanged();
    }
}

ScopedAStatus RadioSim::getIccCardStatus(int32_t serial) {
    LOG_CALL;
    mExec.post([this, serial] {
        if (!QmiModem::get().isReady()) {
            respond()->getIccCardStatusResponse(
                    errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE), {});
            return;
        }
        refreshCardStatus(false);
        auto snap = State::get().sim();
        if (!snap.valid) {
            respond()->getIccCardStatusResponse(errorResponse(serial, RadioError::INTERNAL_ERR),
                                                {});
            return;
        }
        respond()->getIccCardStatusResponse(noError(serial), snap.card);
    });
    return ok();
}

ScopedAStatus RadioSim::getImsiForApp(int32_t serial, const std::string& aid) {
    LOG_CALL << aid;
    mExec.post([this, serial] {
        std::string imsi = readImsi();
        if (imsi.empty()) {
            g_autoptr(GError) err = nullptr;
            auto out = QMI_SYNC(dms, uim_get_imsi, QmiModem::get().dms, nullptr, &err);
            const gchar* i = nullptr;
            if (out && qmi_message_dms_uim_get_imsi_output_get_imsi(out.get(), &i, nullptr) && i)
                imsi = i;
        }
        if (imsi.empty()) {
            LOG(ERROR) << "IMSI not readable";
            respond()->getImsiForAppResponse(errorResponse(serial, RadioError::INTERNAL_ERR), "");
            return;
        }
        respond()->getImsiForAppResponse(noError(serial), imsi);
    });
    return ok();
}

/* Session for an ICC IO request: card level, the GW provisioning app, or another app. */
static void sessionFor(const std::string& aid, QmiUimSessionType* type, GArray** aidArray) {
    auto gwAid = State::get().sim().gwAidHex;
    if (aid.empty()) {
        *type = QMI_UIM_SESSION_TYPE_CARD_SLOT_1;
        *aidArray = g_array_new(FALSE, FALSE, 1);
    } else if (strcasecmp(aid.c_str(), gwAid.c_str()) == 0) {
        *type = QMI_UIM_SESSION_TYPE_PRIMARY_GW_PROVISIONING;
        *aidArray = g_array_new(FALSE, FALSE, 1);
    } else {
        *type = QMI_UIM_SESSION_TYPE_NONPROVISIONING_SLOT_1;
        *aidArray = hexToGArray(aid);
    }
}

static aidl::IccIoResult ioError(guint8 sw1 = 0x6A, guint8 sw2 = 0x82) {
    return {.sw1 = sw1, .sw2 = sw2, .simResponse = ""};
}

aidl::IccIoResult RadioSim::doIccIo(const aidl::IccIo& io) {
    auto& m = QmiModem::get();
    QmiUimSessionType st;
    g_autoptr(GArray) aid = nullptr;
    sessionFor(io.aid, &st, &aid);
    g_autoptr(GArray) path = simPathToGArray(io.path);
    g_autoptr(GError) err = nullptr;
    guint8 sw1 = 0x90, sw2 = 0x00;

    switch (io.command) {
        case 0xB0: { /* READ BINARY */
            auto* in = qmi_message_uim_read_transparent_input_new();
            qmi_message_uim_read_transparent_input_set_session(in, st, aid, nullptr);
            qmi_message_uim_read_transparent_input_set_file(in, io.fileId, path, nullptr);
            qmi_message_uim_read_transparent_input_set_read_information(
                    in, (io.p1 << 8) | io.p2, io.p3, nullptr);
            auto out = QMI_SYNC(uim, read_transparent, m.uim, in, &err);
            qmi_message_uim_read_transparent_input_unref(in);
            if (!out) return ioError();
            GArray* data = nullptr;
            qmi_message_uim_read_transparent_output_get_card_result(out.get(), &sw1, &sw2, nullptr);
            qmi_message_uim_read_transparent_output_get_read_result(out.get(), &data, nullptr);
            return {.sw1 = sw1, .sw2 = sw2, .simResponse = garrayToHex(data)};
        }
        case 0xB2: { /* READ RECORD */
            auto* in = qmi_message_uim_read_record_input_new();
            qmi_message_uim_read_record_input_set_session(in, st, aid, nullptr);
            qmi_message_uim_read_record_input_set_file(in, io.fileId, path, nullptr);
            qmi_message_uim_read_record_input_set_record(in, io.p1, io.p3, nullptr);
            auto out = QMI_SYNC(uim, read_record, m.uim, in, &err);
            qmi_message_uim_read_record_input_unref(in);
            if (!out) return ioError();
            GArray* data = nullptr;
            qmi_message_uim_read_record_output_get_card_result(out.get(), &sw1, &sw2, nullptr);
            qmi_message_uim_read_record_output_get_read_result(out.get(), &data, nullptr);
            return {.sw1 = sw1, .sw2 = sw2, .simResponse = garrayToHex(data)};
        }
        case 0xC0: { /* GET RESPONSE -> synthesize the 3GPP TS 51.011 9.2.1 layout */
            auto* in = qmi_message_uim_get_file_attributes_input_new();
            qmi_message_uim_get_file_attributes_input_set_session(in, st, aid, nullptr);
            qmi_message_uim_get_file_attributes_input_set_file(in, io.fileId, path, nullptr);
            auto out = QMI_SYNC(uim, get_file_attributes, m.uim, in, &err);
            qmi_message_uim_get_file_attributes_input_unref(in);
            if (!out) return ioError();
            guint16 size = 0, id = 0, recSize = 0, recCount = 0;
            QmiUimFileType type = QMI_UIM_FILE_TYPE_TRANSPARENT;
            QmiUimSecurityAttributeLogic l;
            QmiUimSecurityAttribute a;
            GArray* raw = nullptr;
            qmi_message_uim_get_file_attributes_output_get_card_result(out.get(), &sw1, &sw2,
                                                                       nullptr);
            if (!qmi_message_uim_get_file_attributes_output_get_file_attributes(
                        out.get(), &size, &id, &type, &recSize, &recCount, &l, &a, &l, &a, &l, &a,
                        &l, &a, &l, &a, &raw, nullptr)) {
                return ioError();
            }
            std::vector<uint8_t> r(15, 0);
            r[2] = size >> 8;
            r[3] = size & 0xff;
            r[4] = id >> 8;
            r[5] = id & 0xff;
            r[6] = 0x04; /* EF */
            r[12] = 2;
            switch (type) {
                case QMI_UIM_FILE_TYPE_LINEAR_FIXED:
                    r[13] = 0x01;
                    break;
                case QMI_UIM_FILE_TYPE_CYCLIC:
                    r[13] = 0x03;
                    break;
                default:
                    r[13] = 0x00;
            }
            r[14] = recSize & 0xff;
            return {.sw1 = 0x90, .sw2 = 0x00, .simResponse = bytesToHex(r.data(), r.size())};
        }
        case 0xF2: /* STATUS */
            return {.sw1 = 0x90, .sw2 = 0x00, .simResponse = ""};
        default:
            LOG(WARNING) << "iccIo: unsupported command 0x" << std::hex << io.command;
            return ioError(0x6D, 0x00);
    }
}

ScopedAStatus RadioSim::iccIoForApp(int32_t serial, const aidl::IccIo& iccIo) {
    LOG_CALL << std::hex << iccIo.command << " " << iccIo.fileId << " " << iccIo.path;
    mExec.post([this, serial, iccIo] {
        auto res = doIccIo(iccIo);
        respond()->iccIoForAppResponse(noError(serial), res);
    });
    return ok();
}

/*
 * Map a UIM verify/unblock result to what Android expects. Only a real wrong
 * code is PASSWORD_INCORRECT; transport/state errors must not look like one
 * (keyguard would count them and may jump to the PUK screen). "No effect"
 * means the PIN is already verified, which is what the caller wanted.
 * Retries come from the response TLV, else from a fresh card status.
 */
RadioSim::PinResult RadioSim::pinResult(bool success, GError* err, int left, bool puk) {
    PinResult r{RadioError::NONE, left};
    if (!success) {
        if (err && g_error_matches(err, QMI_PROTOCOL_ERROR, QMI_PROTOCOL_ERROR_NO_EFFECT))
            r.error = RadioError::NONE;
        else if (err && (g_error_matches(err, QMI_PROTOCOL_ERROR,
                                         QMI_PROTOCOL_ERROR_INCORRECT_PIN) ||
                         g_error_matches(err, QMI_PROTOCOL_ERROR, QMI_PROTOCOL_ERROR_PIN_BLOCKED) ||
                         g_error_matches(err, QMI_PROTOCOL_ERROR,
                                         QMI_PROTOCOL_ERROR_PIN_ALWAYS_BLOCKED)))
            r.error = RadioError::PASSWORD_INCORRECT;
        else
            r.error = RadioError::INTERNAL_ERR;
    }
    if (r.retries < 0 || r.error != RadioError::PASSWORD_INCORRECT) {
        refreshCardStatus(false);
        auto snap = State::get().sim();
        int fromCard = puk ? snap.puk1Retries : snap.pin1Retries;
        if (r.retries < 0) r.retries = fromCard;
    }
    return r;
}

ScopedAStatus RadioSim::supplyIccPinForApp(int32_t serial, const std::string& pin,
                                           const std::string& aid) {
    LOG_CALL << aid;
    mExec.post([this, serial, pin] {
        auto* in = qmi_message_uim_verify_pin_input_new();
        g_autoptr(GArray) aidArr = g_array_new(FALSE, FALSE, 1);
        qmi_message_uim_verify_pin_input_set_session(
                in, QMI_UIM_SESSION_TYPE_PRIMARY_GW_PROVISIONING, aidArr, nullptr);
        qmi_message_uim_verify_pin_input_set_info(in, QMI_UIM_PIN_ID_PIN1, pin.c_str(), nullptr);
        g_autoptr(GError) err = nullptr;
        /* keep the raw output: retries are reported on failure too */
        GAsyncResult* res = QmiModem::get().call([&](GAsyncReadyCallback cb, gpointer ud) {
            qmi_client_uim_verify_pin(QmiModem::get().uim, in, 10, nullptr, cb, ud);
        });
        qmi_message_uim_verify_pin_input_unref(in);
        QmiMessageUimVerifyPinOutput* out =
                res ? qmi_client_uim_verify_pin_finish(QmiModem::get().uim, res, &err) : nullptr;
        if (res) g_object_unref(res);
        guint8 left = 0, unblockLeft = 0;
        bool haveLeft = false;
        bool success = out && qmi_message_uim_verify_pin_output_get_result(out, &err);
        if (out) {
            haveLeft = qmi_message_uim_verify_pin_output_get_retries_remaining(out, &left,
                                                                               &unblockLeft,
                                                                               nullptr);
            qmi_message_uim_verify_pin_output_unref(out);
        }
        auto r = pinResult(success, err, haveLeft ? left : -1);
        LOG(INFO) << "verify PIN1: " << (success ? "ok" : (err ? err->message : "timeout"))
                  << " -> " << toString(r.error) << ", retries " << r.retries;
        respond()->supplyIccPinForAppResponse(
                r.error == RadioError::NONE ? noError(serial) : errorResponse(serial, r.error),
                r.retries);
        refreshCardStatus(true);
    });
    return ok();
}

ScopedAStatus RadioSim::supplyIccPukForApp(int32_t serial, const std::string& puk,
                                           const std::string& pin, const std::string& aid) {
    LOG_CALL << aid;
    mExec.post([this, serial, puk, pin] {
        auto* in = qmi_message_uim_unblock_pin_input_new();
        g_autoptr(GArray) aidArr = g_array_new(FALSE, FALSE, 1);
        qmi_message_uim_unblock_pin_input_set_session(
                in, QMI_UIM_SESSION_TYPE_PRIMARY_GW_PROVISIONING, aidArr, nullptr);
        qmi_message_uim_unblock_pin_input_set_info(in, QMI_UIM_PIN_ID_PIN1, puk.c_str(),
                                                   pin.c_str(), nullptr);
        g_autoptr(GError) err = nullptr;
        GAsyncResult* res = QmiModem::get().call([&](GAsyncReadyCallback cb, gpointer ud) {
            qmi_client_uim_unblock_pin(QmiModem::get().uim, in, 10, nullptr, cb, ud);
        });
        qmi_message_uim_unblock_pin_input_unref(in);
        QmiMessageUimUnblockPinOutput* out =
                res ? qmi_client_uim_unblock_pin_finish(QmiModem::get().uim, res, &err) : nullptr;
        if (res) g_object_unref(res);
        guint8 left = 0, unblockLeft = 0;
        bool haveLeft = false;
        bool success = out && qmi_message_uim_unblock_pin_output_get_result(out, &err);
        if (out) {
            haveLeft = qmi_message_uim_unblock_pin_output_get_retries_remaining(out, &left,
                                                                                &unblockLeft,
                                                                                nullptr);
            qmi_message_uim_unblock_pin_output_unref(out);
        }
        auto r = pinResult(success, err, haveLeft ? unblockLeft : -1, true);
        LOG(INFO) << "unblock PIN1: " << (success ? "ok" : (err ? err->message : "timeout"))
                  << " -> " << toString(r.error) << ", retries " << r.retries;
        respond()->supplyIccPukForAppResponse(
                r.error == RadioError::NONE ? noError(serial) : errorResponse(serial, r.error),
                r.retries);
        refreshCardStatus(true);
    });
    return ok();
}

ScopedAStatus RadioSim::getFacilityLockForApp(int32_t serial, const std::string& facility,
                                              const std::string& password, int32_t serviceClass,
                                              const std::string& appId) {
    LOG_CALL << facility << " " << serviceClass << " " << appId;
    (void)password;
    mExec.post([this, serial, facility] {
        int locked = 0;
        if (facility == "SC") {
            auto snap = State::get().sim();
            int idx = snap.card.gsmUmtsSubscriptionAppIndex;
            if (idx >= 0 && idx < static_cast<int>(snap.card.applications.size())) {
                auto pin = snap.card.applications[idx].pin1;
                locked = (pin == aidl::PinState::ENABLED_NOT_VERIFIED ||
                          pin == aidl::PinState::ENABLED_VERIFIED ||
                          pin == aidl::PinState::ENABLED_BLOCKED)
                                 ? 1
                                 : 0;
            }
        }
        respond()->getFacilityLockForAppResponse(noError(serial), locked);
    });
    return ok();
}

}  // namespace angler::radio
