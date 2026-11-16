/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <atomic>
#include <map>
#include <mutex>

#include <libminradio/data/RadioData.h>

#include "Qmi.h"

namespace angler::radio {

class RadioData : public ::android::hardware::radio::minimal::RadioData {
  public:
    using ::android::hardware::radio::minimal::RadioData::RadioData;
    void setupIndications();

  protected:
    ::ndk::ScopedAStatus setupDataCall(
            int32_t serial, ::aidl::android::hardware::radio::AccessNetwork accessNetwork,
            const ::aidl::android::hardware::radio::data::DataProfileInfo& dataProfileInfo,
            bool roamingAllowed, ::aidl::android::hardware::radio::data::DataRequestReason reason,
            const std::vector<::aidl::android::hardware::radio::data::LinkAddress>& addresses,
            const std::vector<std::string>& dnses, int32_t pduSessionId,
            const std::optional<::aidl::android::hardware::radio::data::SliceInfo>& sliceInfo,
            bool matchAllRuleAllowed) override;
    ::ndk::ScopedAStatus deactivateDataCall(
            int32_t serial, int32_t cid,
            ::aidl::android::hardware::radio::data::DataRequestReason reason) override;

  private:
    void doSetup(int32_t serial, ::aidl::android::hardware::radio::data::DataProfileInfo profile);
    bool prepareDataPath();
    void dropCall(int32_t cid, bool notify);
    void logPacketStats();
    void startStatsPoller();
    std::atomic<bool> mStatsRunning{false};

    SerialExecutor mExec{"qmiril-data"};
    bool mDataPathReady = false;
    std::mutex mCallLock;
    std::map<int32_t, guint32> mHandles; /* cid -> WDS packet data handle */
    struct IfAddr {
        std::string ifname;
        uint32_t ip = 0;
        int prefix = 0;
    };
    std::map<int32_t, IfAddr> mAddrs; /* cid -> address configured on the link */
};

}  // namespace angler::radio
