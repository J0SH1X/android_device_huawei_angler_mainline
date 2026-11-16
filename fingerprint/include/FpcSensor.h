/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <android-base/unique_fd.h>

#include <string>

namespace angler::fpc {

/**
 * The fpc1020 platform device as exposed by the (future) kernel driver. Attributes, all in one
 * sysfs directory:
 *   clk_enable     "1"/"0"        SPI clocks for the TZ side
 *   irq            read '1' = asserted, sysfs_notify on change, any write acks
 *   wakeup_enable  "enable"/"disable"
 */
class FpcSensor {
  public:
    enum class Wait { IRQ, CANCELLED, TIMEOUT, ERROR };

    /** Finds the sysfs directory (property ro.vendor.fpc.sysfs, else scan by modalias). */
    int open();
    bool isOpen() const { return mDirFd.ok(); }

    int setClock(bool on);
    int setWakeup(bool on);

    /** Waits for the irq attribute to assert, a byte on cancelFd, or timeoutMs (<0: forever). */
    Wait waitIrq(int cancelFd, int timeoutMs);
    int ackIrq();

    /** Holds the SPI clock for the lifetime of the guard. */
    class ClockGuard {
      public:
        explicit ClockGuard(FpcSensor& s) : mS(s) { mS.setClock(true); }
        ~ClockGuard() { mS.setClock(false); }
        ClockGuard(const ClockGuard&) = delete;
        ClockGuard& operator=(const ClockGuard&) = delete;

      private:
        FpcSensor& mS;
    };

    static std::string findSysfsDir();

  private:
    int writeAttr(const char* name, const char* value);
    bool irqAsserted();

    android::base::unique_fd mDirFd;
    android::base::unique_fd mIrqFd;
};

}  // namespace angler::fpc
