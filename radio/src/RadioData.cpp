/*
 * Data: WDS/WDA/DPM over the IPA (QMAP) data path.
 *
 * One-time data path setup (DPM open port, WDA data format) and the rmnet
 * link are tunable with properties, defaults follow what worked by hand on
 * this modem:
 *   persist.vendor.qmiril.data.ipa_iface   rmnet_ipa0   (IPA netdev)
 *   persist.vendor.qmiril.data.ifname      rmnet_data0  (rmnet mux netdev)
 *   persist.vendor.qmiril.data.mux_id      1
 *   persist.vendor.qmiril.data.ep_iface    1            (embedded endpoint)
 *   persist.vendor.qmiril.data.hw_rx/hw_tx 5 / 4
 *   persist.vendor.qmiril.data.qmap        1            (0: raw IP, no mux)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "qmiril"

#include "RadioData.h"

#include <arpa/inet.h>
#include <linux/if_addr.h>
#include <linux/if_link.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include <aidl/android/hardware/radio/RadioConst.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <libminradio/debug.h>
#include <libminradio/response.h>

#define RADIO_MODULE "Data"

namespace angler::radio {

using ::aidl::android::hardware::radio::AccessNetwork;
using ::aidl::android::hardware::radio::RadioConst;
using ::aidl::android::hardware::radio::RadioError;
using ::aidl::android::hardware::radio::RadioIndicationType;
using ::android::base::GetIntProperty;
using ::android::base::GetProperty;
using ::android::hardware::radio::minimal::errorResponse;
using ::android::hardware::radio::minimal::noError;
using ::ndk::ScopedAStatus;
namespace aidl = ::aidl::android::hardware::radio::data;
constexpr auto ok = &ScopedAStatus::ok;

static std::string prop(const char* name, const char* def) {
    return GetProperty(std::string("persist.vendor.qmiril.data.") + name, def);
}
static int propInt(const char* name, int def) {
    return GetIntProperty(std::string("persist.vendor.qmiril.data.") + name, def);
}

/* ---------------- netdev helpers ---------------- */

static bool setIfUp(const std::string& ifname, bool up, int mtu = 0) {
    android::base::unique_fd fd(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    if (fd < 0) return false;
    struct ifreq ifr = {};
    strlcpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ);
    if (mtu > 0) {
        ifr.ifr_mtu = mtu;
        if (ioctl(fd, SIOCSIFMTU, &ifr) < 0) PLOG(WARNING) << "SIOCSIFMTU " << ifname;
    }
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        PLOG(ERROR) << "SIOCGIFFLAGS " << ifname;
        return false;
    }
    if (up)
        ifr.ifr_flags |= IFF_UP;
    else
        ifr.ifr_flags &= ~IFF_UP;
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) {
        PLOG(ERROR) << "SIOCSIFFLAGS " << ifname;
        return false;
    }
    return true;
}

/* ip addr add/del <ip>/<prefix> dev <ifname> (host-order ip). Android (netd)
 * only installs routes and DNS on cellular links; the address is ours. */
static bool ifAddr(const std::string& ifname, uint32_t ip, int prefix, bool add) {
    unsigned idx = if_nametoindex(ifname.c_str());
    if (idx == 0) return false;
    android::base::unique_fd fd(socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE));
    if (fd < 0) return false;
    struct {
        struct nlmsghdr nh;
        struct ifaddrmsg ifa;
        char buf[64];
    } req = {};
    req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    req.nh.nlmsg_type = add ? RTM_NEWADDR : RTM_DELADDR;
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | (add ? NLM_F_CREATE | NLM_F_REPLACE : 0);
    req.ifa.ifa_family = AF_INET;
    req.ifa.ifa_prefixlen = prefix;
    req.ifa.ifa_scope = RT_SCOPE_UNIVERSE;
    req.ifa.ifa_index = idx;
    uint32_t be = htonl(ip);
    for (int type : {IFA_LOCAL, IFA_ADDRESS}) {
        auto* rta = reinterpret_cast<struct rtattr*>(reinterpret_cast<char*>(&req) +
                                                     NLMSG_ALIGN(req.nh.nlmsg_len));
        rta->rta_type = type;
        rta->rta_len = RTA_LENGTH(sizeof(be));
        memcpy(RTA_DATA(rta), &be, sizeof(be));
        req.nh.nlmsg_len = NLMSG_ALIGN(req.nh.nlmsg_len) + RTA_ALIGN(rta->rta_len);
    }
    if (send(fd, &req, req.nh.nlmsg_len, 0) < 0) {
        PLOG(ERROR) << "netlink send (addr)";
        return false;
    }
    char reply[512];
    ssize_t n = recv(fd, reply, sizeof(reply), 0);
    if (n < 0) return false;
    auto* nh = reinterpret_cast<struct nlmsghdr*>(reply);
    if (nh->nlmsg_type == NLMSG_ERROR) {
        auto* e = static_cast<struct nlmsgerr*>(NLMSG_DATA(nh));
        if (e->error != 0) {
            LOG(ERROR) << (add ? "add" : "del") << " address on " << ifname << ": "
                       << strerror(-e->error);
            return false;
        }
    }
    return true;
}

static int getIfMtu(const std::string& ifname) {
    android::base::unique_fd fd(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    if (fd < 0) return 0;
    struct ifreq ifr = {};
    strlcpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ);
    if (ioctl(fd, SIOCGIFMTU, &ifr) < 0) return 0;
    return ifr.ifr_mtu;
}

struct NlReq {
    struct nlmsghdr nh;
    struct ifinfomsg ifi;
    char buf[512];
};

static struct rtattr* addAttr(NlReq* req, int type, const void* data, size_t len) {
    auto* rta = reinterpret_cast<struct rtattr*>(reinterpret_cast<char*>(req) +
                                                 NLMSG_ALIGN(req->nh.nlmsg_len));
    rta->rta_type = type;
    rta->rta_len = RTA_LENGTH(len);
    if (len) memcpy(RTA_DATA(rta), data, len);
    req->nh.nlmsg_len = NLMSG_ALIGN(req->nh.nlmsg_len) + RTA_ALIGN(rta->rta_len);
    return rta;
}

static void endNest(NlReq* req, struct rtattr* nest) {
    nest->rta_len = reinterpret_cast<char*>(req) + req->nh.nlmsg_len - reinterpret_cast<char*>(nest);
}

/* ip link add link <parent> name <name> type rmnet mux_id <id> */
static bool createRmnet(const std::string& parent, const std::string& name, uint16_t muxId) {
    if (if_nametoindex(name.c_str()) != 0) return true;
    unsigned parentIdx = if_nametoindex(parent.c_str());
    if (parentIdx == 0) {
        LOG(ERROR) << "rmnet parent " << parent << " does not exist";
        return false;
    }
    android::base::unique_fd fd(socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE));
    if (fd < 0) return false;

    NlReq req = {};
    req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.nh.nlmsg_type = RTM_NEWLINK;
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    req.ifi.ifi_family = AF_UNSPEC;
    uint32_t link = parentIdx;
    addAttr(&req, IFLA_LINK, &link, sizeof(link));
    addAttr(&req, IFLA_IFNAME, name.c_str(), name.size() + 1);
    auto* linkinfo = addAttr(&req, IFLA_LINKINFO, nullptr, 0);
    addAttr(&req, IFLA_INFO_KIND, "rmnet", 6);
    auto* data = addAttr(&req, IFLA_INFO_DATA, nullptr, 0);
    addAttr(&req, IFLA_RMNET_MUX_ID, &muxId, sizeof(muxId));
    struct ifla_rmnet_flags flags = {
            .flags = RMNET_FLAGS_INGRESS_DEAGGREGATION,
            .mask = RMNET_FLAGS_INGRESS_DEAGGREGATION,
    };
    addAttr(&req, IFLA_RMNET_FLAGS, &flags, sizeof(flags));
    endNest(&req, data);
    endNest(&req, linkinfo);

    if (send(fd, &req, req.nh.nlmsg_len, 0) < 0) {
        PLOG(ERROR) << "netlink send";
        return false;
    }
    char reply[1024];
    ssize_t n = recv(fd, reply, sizeof(reply), 0);
    if (n < 0) return false;
    auto* nh = reinterpret_cast<struct nlmsghdr*>(reply);
    if (nh->nlmsg_type == NLMSG_ERROR) {
        auto* e = static_cast<struct nlmsgerr*>(NLMSG_DATA(nh));
        if (e->error != 0) {
            LOG(ERROR) << "create " << name << ": " << strerror(-e->error);
            return false;
        }
    }
    LOG(INFO) << "created " << name << " on " << parent << " mux " << muxId;
    return true;
}

/* ---------------- QMI data path ---------------- */

void RadioData::setupIndications() {
    auto& m = QmiModem::get();
    if (!m.wds) {
        LOG(ERROR) << "no WDS client, mobile data unavailable";
        return;
    }
    m.onIndication<QmiIndicationWdsPacketServiceStatusOutput>(
            QMI_CLIENT(m.wds), "packet-service-status",
            [this](QmiIndicationWdsPacketServiceStatusOutput* o) {
                QmiWdsConnectionStatus status;
                gboolean reconfig;
                if (!qmi_indication_wds_packet_service_status_output_get_connection_status(
                            o, &status, &reconfig, nullptr))
                    return;
                LOG(INFO) << "packet service status " << status;
                if (status == QMI_WDS_CONNECTION_STATUS_DISCONNECTED) {
                    mExec.post([this] {
                        std::vector<int32_t> cids;
                        {
                            std::lock_guard lk(mCallLock);
                            for (auto& [cid, h] : mHandles) cids.push_back(cid);
                        }
                        for (int32_t cid : cids) dropCall(cid, false);
                        indicate()->dataCallListChanged(RadioIndicationType::UNSOLICITED,
                                                        getDataCallListBase());
                    });
                }
            });
}

bool RadioData::prepareDataPath() {
    if (mDataPathReady) return true;
    auto& m = QmiModem::get();
    const int epIface = propInt("ep_iface", 1);
    const bool qmap = propInt("qmap", 1) != 0;

    /* DPM: tell the modem which embedded endpoint carries data
     * (normally already done by QmiModem together with DATA5_CNTL) */
    if (m.dpm && !m.dpmPortOpen) {
        g_autoptr(GError) err = nullptr;
        auto* in = qmi_message_dpm_open_port_input_new();
        g_autoptr(GArray) hw =
                g_array_new(FALSE, FALSE, sizeof(QmiMessageDpmOpenPortInputHardwareDataPortsElement));
        QmiMessageDpmOpenPortInputHardwareDataPortsElement e{
                .endpoint_type = QMI_DATA_ENDPOINT_TYPE_EMBEDDED,
                .interface_number = static_cast<guint32>(epIface),
                .rx_endpoint_number = static_cast<guint32>(propInt("hw_rx", 5)),
                .tx_endpoint_number = static_cast<guint32>(propInt("hw_tx", 4)),
        };
        g_array_append_val(hw, e);
        qmi_message_dpm_open_port_input_set_hardware_data_ports(in, hw, nullptr);
        auto out = QMI_SYNC(dpm, open_port, m.dpm, in, &err);
        qmi_message_dpm_open_port_input_unref(in);
        if (!out) LOG(WARNING) << "dpm open_port: " << err->message;
    }

    /* WDA: raw IP, QMAP aggregation */
    if (m.wda) {
        g_autoptr(GError) err = nullptr;
        auto* in = qmi_message_wda_set_data_format_input_new();
        qmi_message_wda_set_data_format_input_set_endpoint_info(
                in, QMI_DATA_ENDPOINT_TYPE_EMBEDDED, epIface, nullptr);
        qmi_message_wda_set_data_format_input_set_link_layer_protocol(
                in, QMI_WDA_LINK_LAYER_PROTOCOL_RAW_IP, nullptr);
        auto agg = qmap ? QMI_WDA_DATA_AGGREGATION_PROTOCOL_QMAP
                        : QMI_WDA_DATA_AGGREGATION_PROTOCOL_DISABLED;
        qmi_message_wda_set_data_format_input_set_uplink_data_aggregation_protocol(in, agg, nullptr);
        qmi_message_wda_set_data_format_input_set_downlink_data_aggregation_protocol(in, agg,
                                                                                      nullptr);
        if (qmap) {
            qmi_message_wda_set_data_format_input_set_downlink_data_aggregation_max_datagrams(
                    in, 32, nullptr);
            qmi_message_wda_set_data_format_input_set_downlink_data_aggregation_max_size(
                    in, 16384, nullptr);
        }
        auto out = QMI_SYNC(wda, set_data_format, m.wda, in, &err);
        qmi_message_wda_set_data_format_input_unref(in);
        if (!out) {
            LOG(WARNING) << "wda set_data_format: " << err->message;
        } else {
            /* what the modem actually accepted (it may silently downgrade) */
            QmiWdaLinkLayerProtocol llp = QMI_WDA_LINK_LAYER_PROTOCOL_UNKNOWN;
            QmiWdaDataAggregationProtocol ul = QMI_WDA_DATA_AGGREGATION_PROTOCOL_DISABLED,
                                          dl = QMI_WDA_DATA_AGGREGATION_PROTOCOL_DISABLED;
            guint32 dlDg = 0, dlSz = 0;
            gboolean qos = FALSE;
            qmi_message_wda_set_data_format_output_get_link_layer_protocol(out.get(), &llp, nullptr);
            qmi_message_wda_set_data_format_output_get_uplink_data_aggregation_protocol(out.get(), &ul,
                                                                                        nullptr);
            qmi_message_wda_set_data_format_output_get_downlink_data_aggregation_protocol(out.get(), &dl,
                                                                                          nullptr);
            qmi_message_wda_set_data_format_output_get_downlink_data_aggregation_max_datagrams(
                    out.get(), &dlDg, nullptr);
            qmi_message_wda_set_data_format_output_get_downlink_data_aggregation_max_size(out.get(), &dlSz,
                                                                                          nullptr);
            qmi_message_wda_set_data_format_output_get_qos_format(out.get(), &qos, nullptr);
            LOG(INFO) << "wda data format accepted: link-layer " << llp << " ul-aggr " << ul
                      << " dl-aggr " << dl << " dl max " << dlDg << " dg / " << dlSz << " B, qos "
                      << int(qos) << " (ep iface " << epIface << ")";
        }
    }

    /* WDS: bind our client to the mux channel */
    if (qmap) {
        g_autoptr(GError) err = nullptr;
        auto* in = qmi_message_wds_bind_mux_data_port_input_new();
        qmi_message_wds_bind_mux_data_port_input_set_endpoint_info(
                in, QMI_DATA_ENDPOINT_TYPE_EMBEDDED, epIface, nullptr);
        qmi_message_wds_bind_mux_data_port_input_set_mux_id(in, propInt("mux_id", 1), nullptr);
        qmi_message_wds_bind_mux_data_port_input_set_client_type(in, QMI_WDS_CLIENT_TYPE_TETHERED,
                                                                 nullptr);
        auto out = QMI_SYNC(wds, bind_mux_data_port, m.wds, in, &err);
        qmi_message_wds_bind_mux_data_port_input_unref(in);
        if (!out) {
            LOG(ERROR) << "wds bind_mux_data_port: " << err->message;
            return false;
        }
        LOG(INFO) << "wds bound to mux " << propInt("mux_id", 1) << " on ep iface " << epIface;
    }
    mDataPathReady = true;
    return true;
}

static int prefixFromMask(uint32_t mask) {
    return mask ? __builtin_popcount(mask) : 32;
}

void RadioData::doSetup(int32_t serial, aidl::DataProfileInfo profile) {
    auto& m = QmiModem::get();
    auto fail = [&](aidl::DataCallFailCause cause, const char* why) {
        LOG(ERROR) << "setupDataCall " << profile.apn << ": " << why;
        aidl::SetupDataCallResult r{};
        r.cause = cause;
        r.suggestedRetryTime = 5000;
        r.cid = -1;
        r.active = aidl::SetupDataCallResult::DATA_CONNECTION_STATUS_INACTIVE;
        respond()->setupDataCallResponse(noError(serial), r);
    };
    if (!m.wds) return fail(aidl::DataCallFailCause::ERROR_UNSPECIFIED, "no WDS");
    if (!prepareDataPath()) return fail(aidl::DataCallFailCause::ERROR_UNSPECIFIED, "data path");

    g_autoptr(GError) err = nullptr;
    auto* fam = qmi_message_wds_set_ip_family_input_new();
    qmi_message_wds_set_ip_family_input_set_preference(fam, QMI_WDS_IP_FAMILY_IPV4, nullptr);
    auto famOut = QMI_SYNC(wds, set_ip_family, m.wds, fam, &err);
    qmi_message_wds_set_ip_family_input_unref(fam);
    if (!famOut) LOG(WARNING) << "wds set_ip_family: " << err->message;
    g_clear_error(&err);

    auto* in = qmi_message_wds_start_network_input_new();
    qmi_message_wds_start_network_input_set_apn(in, profile.apn.c_str(), nullptr);
    qmi_message_wds_start_network_input_set_ip_family_preference(in, QMI_WDS_IP_FAMILY_IPV4,
                                                                 nullptr);
    int auth = QMI_WDS_AUTHENTICATION_NONE;
    if (profile.authType == aidl::ApnAuthType::PAP_NO_CHAP ||
        profile.authType == aidl::ApnAuthType::PAP_CHAP)
        auth |= QMI_WDS_AUTHENTICATION_PAP;
    if (profile.authType == aidl::ApnAuthType::NO_PAP_CHAP ||
        profile.authType == aidl::ApnAuthType::PAP_CHAP)
        auth |= QMI_WDS_AUTHENTICATION_CHAP;
    qmi_message_wds_start_network_input_set_authentication_preference(
            in, static_cast<QmiWdsAuthentication>(auth), nullptr);
    if (!profile.user.empty())
        qmi_message_wds_start_network_input_set_username(in, profile.user.c_str(), nullptr);
    if (!profile.password.empty())
        qmi_message_wds_start_network_input_set_password(in, profile.password.c_str(), nullptr);

    GAsyncResult* res = m.call(
            [&](GAsyncReadyCallback cb, gpointer ud) {
                qmi_client_wds_start_network(m.wds, in, 60, nullptr, cb, ud);
            },
            70000);
    qmi_message_wds_start_network_input_unref(in);
    QmiMessageWdsStartNetworkOutput* out =
            res ? qmi_client_wds_start_network_finish(m.wds, res, &err) : nullptr;
    if (res) g_object_unref(res);
    guint32 handle = 0;
    bool started = out && qmi_message_wds_start_network_output_get_result(out, &err) &&
                   qmi_message_wds_start_network_output_get_packet_data_handle(out, &handle,
                                                                               nullptr);
    if (!started) {
        QmiWdsCallEndReason reason = QMI_WDS_CALL_END_REASON_GENERIC_UNSPECIFIED;
        QmiWdsVerboseCallEndReasonType vtype;
        gint16 vreason = 0;
        if (out) {
            qmi_message_wds_start_network_output_get_call_end_reason(out, &reason, nullptr);
            qmi_message_wds_start_network_output_get_verbose_call_end_reason(out, &vtype,
                                                                             &vreason, nullptr);
            qmi_message_wds_start_network_output_unref(out);
        }
        LOG(ERROR) << "start_network: " << (err ? err->message : "timeout") << " (reason "
                   << reason << ", verbose " << vreason << ")";
        return fail(aidl::DataCallFailCause::ERROR_UNSPECIFIED, "start_network");
    }
    qmi_message_wds_start_network_output_unref(out);
    LOG(INFO) << "start_network " << profile.apn << ": handle " << handle;

    g_clear_error(&err);
    auto* sin = qmi_message_wds_get_current_settings_input_new();
    qmi_message_wds_get_current_settings_input_set_requested_settings(
            sin,
            static_cast<QmiWdsRequestedSettings>(
                    QMI_WDS_REQUESTED_SETTINGS_IP_ADDRESS | QMI_WDS_REQUESTED_SETTINGS_GATEWAY_INFO |
                    QMI_WDS_REQUESTED_SETTINGS_DNS_ADDRESS | QMI_WDS_REQUESTED_SETTINGS_MTU |
                    QMI_WDS_REQUESTED_SETTINGS_IP_FAMILY),
            nullptr);
    auto settings = QMI_SYNC(wds, get_current_settings, m.wds, sin, &err);
    qmi_message_wds_get_current_settings_input_unref(sin);
    if (!settings) {
        LOG(ERROR) << "get_current_settings: " << err->message;
        return fail(aidl::DataCallFailCause::ERROR_UNSPECIFIED, "settings");
    }
    guint32 ip = 0, gw = 0, mask = 0, dns1 = 0, dns2 = 0, mtu = 0;
    qmi_message_wds_get_current_settings_output_get_ipv4_address(settings.get(), &ip, nullptr);
    qmi_message_wds_get_current_settings_output_get_ipv4_gateway_address(settings.get(), &gw,
                                                                          nullptr);
    qmi_message_wds_get_current_settings_output_get_ipv4_gateway_subnet_mask(settings.get(), &mask,
                                                                              nullptr);
    qmi_message_wds_get_current_settings_output_get_primary_ipv4_dns_address(settings.get(), &dns1,
                                                                              nullptr);
    qmi_message_wds_get_current_settings_output_get_secondary_ipv4_dns_address(settings.get(),
                                                                                &dns2, nullptr);
    qmi_message_wds_get_current_settings_output_get_mtu(settings.get(), &mtu, nullptr);

    /* Linux side: rmnet mux device on top of the IPA netdev */
    std::string ipaIf = prop("ipa_iface", "rmnet_ipa0");
    std::string ifname = prop("ifname", "rmnet_data0");
    if (propInt("qmap", 1)) {
        setIfUp(ipaIf, true);
        if (!createRmnet(ipaIf, ifname, propInt("mux_id", 1)))
            LOG(ERROR) << "rmnet link setup failed, data will not flow";
    } else {
        ifname = ipaIf;
    }
    /* rmnet MTU is capped at parent MTU - MAP header (IPA: 1500 - 4); never
     * report more than the link really takes, netd's set-MTU would fail */
    int linkMtu = getIfMtu(ifname);
    if (mtu == 0 || mtu >= 65536) mtu = linkMtu;
    if (linkMtu > 0 && static_cast<int>(mtu) > linkMtu) mtu = linkMtu;
    setIfUp(ifname, true, static_cast<int>(mtu) != linkMtu ? mtu : 0);
    const int prefix = prefixFromMask(mask);
    if (!ifAddr(ifname, ip, prefix, true))
        LOG(ERROR) << "could not configure " << ipv4ToString(ip) << " on " << ifname;

    aidl::SetupDataCallResult r{};
    r.cause = aidl::DataCallFailCause::NONE;
    r.suggestedRetryTime = RadioConst::VALUE_UNAVAILABLE_LONG;
    r.cid = setupDataCallCid();
    r.active = aidl::SetupDataCallResult::DATA_CONNECTION_STATUS_ACTIVE;
    r.type = aidl::PdpProtocolType::IP;
    r.ifname = ifname;
    r.addresses = {{
            .address = ipv4ToString(ip) + "/" + std::to_string(prefix),
            .addressProperties = 0,
            .deprecationTime = RadioConst::VALUE_UNAVAILABLE_LONG,
            .expirationTime = RadioConst::VALUE_UNAVAILABLE_LONG,
    }};
    if (dns1) r.dnses.push_back(ipv4ToString(dns1));
    if (dns2) r.dnses.push_back(ipv4ToString(dns2));
    if (gw) r.gateways.push_back(ipv4ToString(gw));
    r.mtuV4 = mtu > 0 && mtu < 65536 ? mtu : 0;
    r.mtuV6 = 0;
    r.handoverFailureMode = aidl::SetupDataCallResult::HANDOVER_FAILURE_MODE_LEGACY;
    r.pduSessionId = 0;
    LOG(INFO) << "data call up: " << ifname << " " << r.addresses[0].address << " gw "
              << (gw ? ipv4ToString(gw) : "-") << " mtu " << mtu;
    {
        std::lock_guard lk(mCallLock);
        mHandles[r.cid] = handle;
        mAddrs[r.cid] = {ifname, ip, prefix};
    }
    setupDataCallBase(r);
    respond()->setupDataCallResponse(noError(serial), r);
    startStatsPoller();
}

/* DEBUG: modem-side packet counters while a call is up (does the modem see our
 * uplink / get downlink from the network?) */
void RadioData::logPacketStats() {
    auto& m = QmiModem::get();
    if (!m.wds) return;
    g_autoptr(GError) err = nullptr;
    auto* in = qmi_message_wds_get_packet_statistics_input_new();
    qmi_message_wds_get_packet_statistics_input_set_mask(
            in, static_cast<QmiWdsPacketStatisticsMaskFlag>(0x3ff), nullptr);
    auto out = QMI_SYNC(wds, get_packet_statistics, m.wds, in, &err);
    qmi_message_wds_get_packet_statistics_input_unref(in);
    if (!out) {
        LOG(WARNING) << "wds get_packet_statistics: " << err->message;
        return;
    }
    guint32 txOk = 0, rxOk = 0, txErr = 0, rxErr = 0, txDrop = 0, rxDrop = 0;
    guint64 txB = 0, rxB = 0;
    qmi_message_wds_get_packet_statistics_output_get_tx_packets_ok(out.get(), &txOk, nullptr);
    qmi_message_wds_get_packet_statistics_output_get_rx_packets_ok(out.get(), &rxOk, nullptr);
    qmi_message_wds_get_packet_statistics_output_get_tx_packets_error(out.get(), &txErr, nullptr);
    qmi_message_wds_get_packet_statistics_output_get_rx_packets_error(out.get(), &rxErr, nullptr);
    qmi_message_wds_get_packet_statistics_output_get_tx_packets_dropped(out.get(), &txDrop, nullptr);
    qmi_message_wds_get_packet_statistics_output_get_rx_packets_dropped(out.get(), &rxDrop, nullptr);
    qmi_message_wds_get_packet_statistics_output_get_tx_bytes_ok(out.get(), &txB, nullptr);
    qmi_message_wds_get_packet_statistics_output_get_rx_bytes_ok(out.get(), &rxB, nullptr);
    LOG(INFO) << "modem pkt stats: tx ok " << txOk << " (" << txB << " B) err " << txErr
              << " drop " << txDrop << " | rx ok " << rxOk << " (" << rxB << " B) err " << rxErr
              << " drop " << rxDrop;
}

void RadioData::startStatsPoller() {
    if (mStatsRunning.exchange(true)) return;
    std::thread([this] {
        for (int i = 0; i < 360; i++) { /* ~1 h at 10 s */
            std::this_thread::sleep_for(std::chrono::seconds(10));
            {
                std::lock_guard lk(mCallLock);
                if (mHandles.empty()) break;
            }
            mExec.post([this] { logPacketStats(); });
        }
        mStatsRunning = false;
    }).detach();
}

ScopedAStatus RadioData::setupDataCall(int32_t serial, AccessNetwork accessNetwork,
                                       const aidl::DataProfileInfo& dataProfileInfo,
                                       bool roamingAllowed, aidl::DataRequestReason reason,
                                       const std::vector<aidl::LinkAddress>&,
                                       const std::vector<std::string>&, int32_t,
                                       const std::optional<aidl::SliceInfo>&, bool) {
    LOG_CALL << static_cast<int>(accessNetwork) << " " << dataProfileInfo.apn << " "
             << roamingAllowed << " " << static_cast<int>(reason);
    mExec.post([this, serial, dataProfileInfo] { doSetup(serial, dataProfileInfo); });
    return ok();
}

void RadioData::dropCall(int32_t cid, bool stopNetwork) {
    guint32 handle = 0;
    IfAddr addr;
    {
        std::lock_guard lk(mCallLock);
        auto it = mHandles.find(cid);
        if (it == mHandles.end()) return;
        handle = it->second;
        mHandles.erase(it);
        auto a = mAddrs.find(cid);
        if (a != mAddrs.end()) {
            addr = a->second;
            mAddrs.erase(a);
        }
    }
    if (!addr.ifname.empty()) ifAddr(addr.ifname, addr.ip, addr.prefix, false);
    if (stopNetwork) {
        g_autoptr(GError) err = nullptr;
        auto* in = qmi_message_wds_stop_network_input_new();
        qmi_message_wds_stop_network_input_set_packet_data_handle(in, handle, nullptr);
        auto out = QMI_SYNC(wds, stop_network, QmiModem::get().wds, in, &err);
        qmi_message_wds_stop_network_input_unref(in);
        if (!out) LOG(WARNING) << "stop_network: " << err->message;
    }
    deactivateDataCallBase(cid);
}

ScopedAStatus RadioData::deactivateDataCall(int32_t serial, int32_t cid,
                                            aidl::DataRequestReason reason) {
    LOG_CALL << cid << " " << static_cast<int>(reason);
    mExec.post([this, serial, cid] {
        dropCall(cid, true);
        respond()->deactivateDataCallResponse(noError(serial));
    });
    return ok();
}

}  // namespace angler::radio
