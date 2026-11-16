/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <android-base/unique_fd.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "FsListener.h"
#include "RpmbListener.h"
#include "TaChannel.h"

namespace angler::fpc {

/**
 * TaChannel on top of /dev/qseecom-lite (kernel: drivers/firmware/qcom/qcom_qseecom_lite.c,
 * ABI: qseecom_lite_uapi.h, a verbatim copy of the kernel's include/uapi/linux/qseecom_lite.h).
 * The kernel loads the TA itself (request_firmware "<name>.mdt"/".mbn").
 */
class QseecomLiteChannel : public TaChannel {
  public:
    static constexpr size_t kShmSize = 64 * 1024;       // one allocation per client, max command
    static constexpr size_t kListenerBuf = 0x5000;      // stock qseecomd FS listener buffer

    explicit QseecomLiteChannel(FsListener& fs, std::string taName = "fingerpr");
    ~QseecomLiteChannel() override;

    int open() override;
    void close() override;
    int transact(uint8_t* buf, size_t len) override;
    int getAuthTokenKey(std::vector<uint8_t>* key) override;
    int keymasterCall(const std::vector<uint8_t>& cmd, size_t rspCap,
                      std::vector<uint8_t>* rsp) override;

  private:
    void listenerLoop();
    void rpmbLoop();
    void gpfsLoop();
    std::thread mGpfsThread;
    void probeLoop(uint32_t id);
    RpmbListener mRpmb;
    std::thread mRpmbThread;  // debug: log-only listener (FPC_PROBE_LISTENERS)
    std::vector<std::thread> mProbes;

    FsListener& mFs;
    std::string mName;

    std::mutex mLock;
    android::base::unique_fd mDev;
    uint32_t mAppId = 0;
    bool mLoaded = false;
    uint8_t* mShm = nullptr;

    std::thread mListener;
    std::atomic<bool> mStop{false};
};

}  // namespace angler::fpc
