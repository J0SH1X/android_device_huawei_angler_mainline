/*
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "qmiril"

#include "Qmi.h"

#include <arpa/inet.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include <android-base/properties.h>

#include <android-base/logging.h>

namespace angler::radio {

using namespace std::chrono_literals;

/* ---------------- SerialExecutor ---------------- */

SerialExecutor::SerialExecutor(std::string name)
    : mName(std::move(name)), mThread([this] { loop(); }) {}

SerialExecutor::~SerialExecutor() {
    {
        std::lock_guard lk(mLock);
        mStop = true;
    }
    mCond.notify_all();
    if (mThread.joinable()) mThread.join();
}

void SerialExecutor::post(std::function<void()> job) {
    {
        std::lock_guard lk(mLock);
        mJobs.push_back(std::move(job));
    }
    mCond.notify_one();
}

void SerialExecutor::loop() {
    pthread_setname_np(pthread_self(), mName.substr(0, 15).c_str());
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock lk(mLock);
            mCond.wait(lk, [this] { return mStop || !mJobs.empty(); });
            if (mStop && mJobs.empty()) return;
            job = std::move(mJobs.front());
            mJobs.pop_front();
        }
        job();
    }
}

/* ---------------- QmiModem ---------------- */

QmiModem& QmiModem::get() {
    static QmiModem instance;
    return instance;
}

static thread_local bool tOnGlibThread = false;

void QmiModem::glibThread() {
    pthread_setname_np(pthread_self(), "qmi-glib");
    tOnGlibThread = true;
    g_main_context_push_thread_default(mContext);
    g_main_loop_run(mLoop);
    LOG(FATAL) << "GLib main loop exited";
}

void QmiModem::start() {
    mContext = g_main_context_new();
    mLoop = g_main_loop_new(mContext, FALSE);
    mGlibThread = std::thread([this] { glibThread(); });

    mConnectThread = std::thread([this] {
        pthread_setname_np(pthread_self(), "qmi-connect");
        for (int attempt = 1;; attempt++) {
            if (connectOnce()) break;
            LOG(WARNING) << "QMI connect attempt " << attempt << " failed, retrying";
            std::this_thread::sleep_for(attempt < 20 ? 3s : 10s);
        }
        LOG(INFO) << "QMI modem ready";
        mReady = true;
        std::vector<std::function<void()>> cbs;
        {
            std::lock_guard lk(mReadyLock);
            cbs = mReadyCallbacks;
        }
        for (auto& cb : cbs) cb();
    });
}

void QmiModem::onReady(std::function<void()> cb) {
    std::lock_guard lk(mReadyLock);
    mReadyCallbacks.push_back(std::move(cb));
}

void QmiModem::runOnLoop(std::function<void()> fn) {
    if (tOnGlibThread) {
        fn();
        return;
    }
    struct Job {
        std::function<void()> fn;
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
    };
    auto job = std::make_shared<Job>();
    job->fn = std::move(fn);
    auto* holder = new std::shared_ptr<Job>(job);
    g_main_context_invoke_full(
            mContext, G_PRIORITY_DEFAULT,
            [](gpointer ud) -> gboolean {
                auto j = *static_cast<std::shared_ptr<Job>*>(ud);
                j->fn();
                std::lock_guard lk(j->m);
                j->done = true;
                j->cv.notify_all();
                return G_SOURCE_REMOVE;
            },
            holder, [](gpointer ud) { delete static_cast<std::shared_ptr<Job>*>(ud); });
    std::unique_lock lk(job->m);
    job->cv.wait(lk, [&] { return job->done; });
}

GAsyncResult* QmiModem::call(std::function<void(GAsyncReadyCallback, gpointer)> start,
                             unsigned timeoutMs) {
    CHECK(!tOnGlibThread) << "QmiModem::call() on the GLib thread would deadlock";
    struct Waiter {
        std::mutex m;
        std::condition_variable cv;
        GAsyncResult* res = nullptr;
        bool done = false;
    };
    auto waiter = std::make_shared<Waiter>();
    runOnLoop([&] {
        auto* holder = new std::shared_ptr<Waiter>(waiter);
        start(
                [](GObject*, GAsyncResult* res, gpointer ud) {
                    auto* h = static_cast<std::shared_ptr<Waiter>*>(ud);
                    auto w = *h;
                    delete h;
                    std::lock_guard lk(w->m);
                    if (w->done) return; /* caller already gave up */
                    w->res = G_ASYNC_RESULT(g_object_ref(res));
                    w->done = true;
                    w->cv.notify_all();
                },
                holder);
    });
    std::unique_lock lk(waiter->m);
    if (!waiter->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                             [&] { return waiter->done; })) {
        waiter->done = true;
        return nullptr;
    }
    return waiter->res;
}

/*
 * msm8994 modem: only DPM (and IPA/SSCTL/...) are published over QRTR.
 * DMS/NAS/UIM/WMS/VOICE/WDS/WDA live behind the QMUX control port
 * DATA5_CNTL, which the modem only opens after a DPM OPEN_PORT over QRTR
 * (the DPM client has to stay allocated). rpmsg_wwan_ctrl then exposes it
 * as /dev/wwan0qmi0.
 *
 * The QRTR QmiDevice is reused across attempts and closed (not just
 * unreffed) once the port is open: leaving one behind per attempt leaked
 * kernel ns lookups until the global QRTR_NS_MAX_LOOKUPS (128) was hit and
 * nothing could resolve services any more.
 */
static constexpr const char* kQmuxPath = "/dev/wwan0qmi0";

static int dataPropInt(const char* name, int def) {
    return android::base::GetIntProperty(std::string("persist.vendor.qmiril.data.") + name, def);
}

QmiClient* QmiModem::allocateOn(QmiDevice* device, QmiService service, bool required) {
    g_autoptr(GError) err = nullptr;
    GAsyncResult* res = call([&](GAsyncReadyCallback cb, gpointer ud) {
        qmi_device_allocate_client(device, service, QMI_CID_NONE, 10, nullptr, cb, ud);
    });
    QmiClient* client = res ? qmi_device_allocate_client_finish(device, res, &err) : nullptr;
    if (res) g_object_unref(res);
    if (!client) {
        LOG(required ? ERROR : WARNING)
                << "allocate " << qmi_service_get_string(service) << ": "
                << (err ? err->message : "timeout");
    }
    return client;
}

QmiClient* QmiModem::allocate(QmiService service, bool required) {
    return allocateOn(mDevice, service, required);
}

bool QmiModem::openQmuxPort() {
    g_autoptr(GError) err = nullptr;
    auto* in = qmi_message_dpm_open_port_input_new();
    /* DATA5_CNTL is a *control* port (qmicli ctrl-port-name); putting it in the
     * Software Data Ports TLV is accepted by the modem but never opens it. */
    g_autoptr(GArray) ctrl =
            g_array_new(FALSE, FALSE, sizeof(QmiMessageDpmOpenPortInputControlPortsElement));
    QmiMessageDpmOpenPortInputControlPortsElement c{
            .port_name = const_cast<gchar*>("DATA5_CNTL"),
            .endpoint_type = QMI_DATA_ENDPOINT_TYPE_EMBEDDED,
            .interface_number = static_cast<guint32>(dataPropInt("ep_iface", 1)),
    };
    g_array_append_val(ctrl, c);
    qmi_message_dpm_open_port_input_set_control_ports(in, ctrl, nullptr);

    g_autoptr(GArray) hw =
            g_array_new(FALSE, FALSE, sizeof(QmiMessageDpmOpenPortInputHardwareDataPortsElement));
    QmiMessageDpmOpenPortInputHardwareDataPortsElement h{
            .endpoint_type = QMI_DATA_ENDPOINT_TYPE_EMBEDDED,
            .interface_number = static_cast<guint32>(dataPropInt("ep_iface", 1)),
            .rx_endpoint_number = static_cast<guint32>(dataPropInt("hw_rx", 5)),
            .tx_endpoint_number = static_cast<guint32>(dataPropInt("hw_tx", 4)),
    };
    g_array_append_val(hw, h);
    qmi_message_dpm_open_port_input_set_hardware_data_ports(in, hw, nullptr);

    auto out = qmiSync(dpm, qmi_client_dpm_open_port, qmi_client_dpm_open_port_finish,
                       qmi_message_dpm_open_port_output_get_result,
                       qmi_message_dpm_open_port_output_unref, in, &err);
    qmi_message_dpm_open_port_input_unref(in);
    if (!out) {
        LOG(ERROR) << "dpm open_port: " << err->message;
        return false;
    }
    LOG(INFO) << "DPM opened DATA5_CNTL + hw data port";
    dpmPortOpen = true;
    /*
     * The modem only creates the DATA5_CNTL SMD channel once the DPM
     * client is gone (qmicli exiting did it; re-sending OPEN_PORT on a
     * held client never did). Release the CID and close the QRTR device,
     * which also closes its socket and frees its ns lookups.
     */
    QmiDevice* qrtr = mQrtrDevice;
    QmiClientDpm* client = dpm;
    dpm = nullptr;
    mQrtrDevice = nullptr;
    GAsyncResult* res = call([&](GAsyncReadyCallback cb, gpointer ud) {
        qmi_device_release_client(qrtr, QMI_CLIENT(client),
                                  QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID, 5, nullptr, cb, ud);
    });
    if (res) {
        g_autoptr(GError) rerr = nullptr;
        if (!qmi_device_release_client_finish(qrtr, res, &rerr))
            LOG(WARNING) << "dpm release: " << rerr->message;
        g_object_unref(res);
    }
    res = call([&](GAsyncReadyCallback cb, gpointer ud) {
        qmi_device_close_async(qrtr, 5, nullptr, cb, ud);
    });
    if (res) {
        g_autoptr(GError) cerr = nullptr;
        qmi_device_close_finish(qrtr, res, &cerr);
        g_object_unref(res);
    }
    runOnLoop([&] {
        g_object_unref(client);
        g_object_unref(qrtr);
        /* closing the bus socket frees its kernel ns lookups (they are only
         * dropped on socket close; they otherwise pile up per attempt) */
        g_clear_object(&mBus);
    });
    return true;
}

bool QmiModem::connectOnce() {
    g_autoptr(GError) err = nullptr;

    if (!mBus) {
        GAsyncResult* res = call([](GAsyncReadyCallback cb, gpointer ud) {
            qrtr_bus_new(1000, nullptr, cb, ud);
        });
        if (!res) return false;
        mBus = qrtr_bus_new_finish(res, &err);
        g_object_unref(res);
        if (!mBus) {
            LOG(ERROR) << "QRTR bus: " << err->message;
            return false;
        }
    }

    /* 1. QRTR device + DPM client, only while DATA5_CNTL is missing */
    if (access(kQmuxPath, F_OK) == 0) goto qmux;
    if (!mQrtrDevice) {
        GAsyncResult* res = call([this](GAsyncReadyCallback cb, gpointer ud) {
            qrtr_bus_wait_for_node(mBus, 0, 10000, nullptr, cb, ud);
        });
        if (!res) return false;
        g_autoptr(QrtrNode) node = qrtr_bus_wait_for_node_finish(mBus, res, &err);
        g_object_unref(res);
        if (!node) {
            LOG(WARNING) << "modem QRTR node 0 not up: " << err->message;
            return false;
        }
        res = call([&](GAsyncReadyCallback cb, gpointer ud) {
            qmi_device_new_from_node(node, nullptr, cb, ud);
        });
        if (!res) return false;
        QmiDevice* device = qmi_device_new_from_node_finish(res, &err);
        g_object_unref(res);
        if (!device) {
            LOG(ERROR) << "QRTR QmiDevice: " << err->message;
            return false;
        }
        res = call([device](GAsyncReadyCallback cb, gpointer ud) {
            qmi_device_open(device, QMI_DEVICE_OPEN_FLAGS_NONE, 15, nullptr, cb, ud);
        });
        gboolean opened = res && qmi_device_open_finish(device, res, &err);
        if (res) g_object_unref(res);
        if (!opened) {
            LOG(ERROR) << "QRTR open: " << (err ? err->message : "timeout");
            g_object_unref(device);
            return false;
        }
        mQrtrDevice = device;
    }
    if (!dpm) {
        /* DPM is announced a little after the node comes up */
        dpm = QMI_CLIENT_DPM(allocateOn(mQrtrDevice, QMI_SERVICE_DPM, true));
        if (!dpm) return false;
    }

qmux:
    /* 2. DPM open port -> DATA5_CNTL -> /dev/wwan0qmi0 */
    if (access(kQmuxPath, F_OK) != 0) {
        if (!openQmuxPort()) return false;
    }
    for (int i = 0; i < 100 && access(kQmuxPath, R_OK | W_OK) != 0; i++)
        std::this_thread::sleep_for(100ms);
    if (access(kQmuxPath, R_OK | W_OK) != 0) {
        LOG(ERROR) << kQmuxPath << " not usable: " << strerror(errno);
        /* port may have been opened by someone else / lost: retry DPM next time */
        dpmPortOpen = false;
        return false;
    }

    /* 3. QMUX device on the control port */
    {
        g_autoptr(GFile) file = g_file_new_for_path(kQmuxPath);
        GAsyncResult* res = call([&](GAsyncReadyCallback cb, gpointer ud) {
            qmi_device_new(file, nullptr, cb, ud);
        });
        if (!res) return false;
        QmiDevice* device = qmi_device_new_finish(res, &err);
        g_object_unref(res);
        if (!device) {
            LOG(ERROR) << "QMUX QmiDevice: " << err->message;
            return false;
        }
        res = call([device](GAsyncReadyCallback cb, gpointer ud) {
            qmi_device_open(device, QMI_DEVICE_OPEN_FLAGS_NONE, 15, nullptr, cb, ud);
        });
        gboolean opened = res && qmi_device_open_finish(device, res, &err);
        if (res) g_object_unref(res);
        if (!opened) {
            LOG(ERROR) << "QMUX open: " << (err ? err->message : "timeout");
            g_object_unref(device);
            return false;
        }
        mDevice = device;
    }

    dms = QMI_CLIENT_DMS(allocate(QMI_SERVICE_DMS, true));
    nas = QMI_CLIENT_NAS(allocate(QMI_SERVICE_NAS, true));
    uim = QMI_CLIENT_UIM(allocate(QMI_SERVICE_UIM, true));
    if (!dms || !nas || !uim) {
        closeQmux();
        return false;
    }
    wms = QMI_CLIENT_WMS(allocate(QMI_SERVICE_WMS, false));
    voice = QMI_CLIENT_VOICE(allocate(QMI_SERVICE_VOICE, false));
    wds = QMI_CLIENT_WDS(allocate(QMI_SERVICE_WDS, false));
    wda = QMI_CLIENT_WDA(allocate(QMI_SERVICE_WDA, false));
    LOG(INFO) << "QMI clients on " << kQmuxPath << ": dms nas uim" << (wms ? " wms" : "")
              << (voice ? " voice" : "") << (wds ? " wds" : "") << (wda ? " wda" : "");
    return true;
}

void QmiModem::closeQmux() {
    QmiDevice* device = mDevice;
    runOnLoop([&] {
        /* release CIDs on the modem, then drop our refs */
        for (QmiClient** c : {reinterpret_cast<QmiClient**>(&dms), reinterpret_cast<QmiClient**>(&nas),
                              reinterpret_cast<QmiClient**>(&uim)}) {
            if (*c && device)
                qmi_device_release_client(device, *c, QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID,
                                          5, nullptr, nullptr, nullptr);
            g_clear_object(c);
        }
        if (device) qmi_device_close_async(device, 5, nullptr, nullptr, nullptr);
    });
    g_clear_object(&mDevice);
}

/* ---------------- helpers ---------------- */

std::string bytesToHex(const uint8_t* data, size_t len) {
    static const char* digits = "0123456789ABCDEF";
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        s += digits[data[i] >> 4];
        s += digits[data[i] & 0xf];
    }
    return s;
}

std::string garrayToHex(GArray* array) {
    if (!array) return "";
    return bytesToHex(reinterpret_cast<const uint8_t*>(array->data), array->len);
}

std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

GArray* hexToGArray(const std::string& hex) {
    auto bytes = hexToBytes(hex);
    GArray* a = g_array_sized_new(FALSE, FALSE, 1, bytes.size());
    if (!bytes.empty()) g_array_append_vals(a, bytes.data(), bytes.size());
    return a;
}

GArray* simPathToGArray(const std::string& path) {
    GArray* a = g_array_new(FALSE, FALSE, 1);
    for (size_t i = 0; i + 3 < path.size(); i += 4) {
        unsigned v = std::stoul(path.substr(i, 4), nullptr, 16);
        guint8 lo = v & 0xff, hi = (v >> 8) & 0xff;
        g_array_append_val(a, lo);
        g_array_append_val(a, hi);
    }
    return a;
}

std::string ipv4ToString(uint32_t addr) {
    /* libqmi returns host-order integers, most significant byte first */
    char buf[INET_ADDRSTRLEN];
    uint32_t be = htonl(addr);
    inet_ntop(AF_INET, &be, buf, sizeof(buf));
    return buf;
}

std::string ipv6ToString(GArray* addr) {
    if (!addr || addr->len < 8) return "";
    /* libqmi: array of 8 guint16 in host order */
    uint8_t raw[16];
    for (int i = 0; i < 8; i++) {
        uint16_t v = g_array_index(addr, guint16, i);
        raw[i * 2] = v >> 8;
        raw[i * 2 + 1] = v & 0xff;
    }
    char buf[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, raw, buf, sizeof(buf));
    return buf;
}

}  // namespace angler::radio
