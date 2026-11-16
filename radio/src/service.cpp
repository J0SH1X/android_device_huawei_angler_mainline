/*
 * Radio HAL for Qualcomm modems reached over QRTR (QMI), built on libminradio
 * and libqmi. Written for the Nexus 6P (msm8994) on a mainline kernel.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "qmiril"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include "Qmi.h"
#include "RadioConfig.h"
#include "RadioData.h"
#include "RadioMessaging.h"
#include "RadioModem.h"
#include "RadioNetwork.h"
#include "RadioSim.h"
#include "RadioVoice.h"

using namespace std::string_literals;
using ::android::hardware::radio::minimal::SlotContext;

namespace angler::radio {

static std::vector<std::shared_ptr<ndk::ICInterface>> gHals;

template <typename T>
static std::shared_ptr<T> publish(const std::string& instanceSuffix,
                                  std::shared_ptr<SlotContext> context) {
    const auto instance = T::descriptor + "/"s + instanceSuffix;
    if (!AServiceManager_isDeclared(instance.c_str())) {
        LOG(WARNING) << instance << " not declared in VINTF, skipping";
        return nullptr;
    }
    auto hal = ndk::SharedRefBase::make<T>(context);
    gHals.push_back(hal);
    context->addHal(hal);
    CHECK_EQ(AServiceManager_addService(hal->asBinder().get(), instance.c_str()), STATUS_OK);
    LOG(INFO) << "published " << instance;
    return hal;
}

static void main() {
    android::base::InitLogging(nullptr, android::base::LogdLogger(android::base::RADIO));
    android::base::SetDefaultTag("qmiril");
    android::base::SetMinimumLogSeverity(android::base::DEBUG);
    LOG(INFO) << "QMI radio HAL starting";

    /* Worker threads block on the modem, binder threads never do. */
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();

    auto slot = std::make_shared<SlotContext>(1);
    auto config = ndk::SharedRefBase::make<RadioConfig>();
    gHals.push_back(config);
    CHECK_EQ(AServiceManager_addService(config->asBinder().get(),
                                        (RadioConfig::descriptor + "/default"s).c_str()),
             STATUS_OK);

    auto sim = publish<RadioSim>("slot1", slot);
    auto modem = publish<RadioModem>("slot1", slot);
    auto network = publish<RadioNetwork>("slot1", slot);
    auto data = publish<RadioData>("slot1", slot);
    auto messaging = publish<RadioMessaging>("slot1", slot);
    auto voice = publish<RadioVoice>("slot1", slot);

    auto& qmi = QmiModem::get();
    qmi.onReady([=] {
        auto& q = QmiModem::get();
        /* Runs on the connect thread: synchronous QMI calls are fine here. */
        if (sim) {
            g_autoptr(GError) err = nullptr;
            auto* in = qmi_message_uim_register_events_input_new();
            qmi_message_uim_register_events_input_set_event_registration_mask(
                    in, QMI_UIM_EVENT_REGISTRATION_FLAG_CARD_STATUS, nullptr);
            auto out = QMI_SYNC(uim, register_events, q.uim, in, &err);
            qmi_message_uim_register_events_input_unref(in);
            if (!out) LOG(WARNING) << "uim register_events: " << err->message;
            q.onIndication<QmiIndicationUimCardStatusOutput>(
                    QMI_CLIENT(q.uim), "card-status",
                    [sim](QmiIndicationUimCardStatusOutput*) { sim->postRefresh(); });
            sim->refreshCardStatus(false);
        }
        if (network) network->setupIndications();
        if (messaging) messaging->setupIndications();
        if (voice) voice->setupIndications();
        if (data) data->setupIndications();
        slot->setConnected();
        LOG(INFO) << "radio HAL connected to the modem";
    });
    qmi.start();

    ABinderProcess_joinThreadPool();
    LOG(FATAL) << "radio HAL binder thread pool exited";
}

}  // namespace angler::radio

int main() {
    angler::radio::main();
    return EXIT_FAILURE;
}
