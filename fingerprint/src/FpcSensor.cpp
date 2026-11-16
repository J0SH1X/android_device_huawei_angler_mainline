/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "fpc-angler-sensor"

#include "FpcSensor.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace angler::fpc {

namespace {
const char* kScanRoots[] = {"/sys/bus/platform/devices", "/sys/bus/spi/devices"};
}

std::string FpcSensor::findSysfsDir() {
    std::string forced = android::base::GetProperty("ro.vendor.fpc.sysfs", "");
    if (!forced.empty()) return forced;

    for (const char* root : kScanRoots) {
        DIR* d = opendir(root);
        if (!d) continue;
        std::string found;
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::string dir = std::string(root) + "/" + e->d_name;
            std::string modalias;
            android::base::ReadFileToString(dir + "/modalias", &modalias);
            if (modalias.find("fpc1020") != std::string::npos ||
                strstr(e->d_name, "fpc1020") != nullptr) {
                found = dir;
                break;
            }
        }
        closedir(d);
        if (!found.empty()) return found;
    }
    return "";
}

int FpcSensor::open() {
    std::string dir = findSysfsDir();
    if (dir.empty()) {
        LOG(ERROR) << "no fpc1020 device found (is the kernel driver loaded?)";
        return -ENODEV;
    }
    mDirFd.reset(::open(dir.c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC));
    if (!mDirFd.ok()) {
        PLOG(ERROR) << "open " << dir;
        return -errno;
    }
    mIrqFd.reset(openat(mDirFd.get(), "irq", O_RDWR | O_CLOEXEC));
    if (!mIrqFd.ok()) {
        PLOG(ERROR) << "open " << dir << "/irq";
        mDirFd.reset();
        return -errno;
    }
    LOG(INFO) << "using " << dir;
    return 0;
}

int FpcSensor::writeAttr(const char* name, const char* value) {
    int fd = openat(mDirFd.get(), name, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        PLOG(ERROR) << "open " << name;
        return -errno;
    }
    ssize_t n = write(fd, value, strlen(value));
    int err = n < 0 ? -errno : 0;
    close(fd);
    if (err) LOG(ERROR) << "write " << name << ": " << strerror(-err);
    return err;
}

int FpcSensor::setClock(bool on) { return writeAttr("clk_enable", on ? "1" : "0"); }
int FpcSensor::setWakeup(bool on) { return writeAttr("wakeup_enable", on ? "enable" : "disable"); }

bool FpcSensor::irqAsserted() {
    char c = 0;
    // Re-reading from offset 0 is also what re-arms sysfs_notify() for poll().
    return pread(mIrqFd.get(), &c, 1, 0) == 1 && c == '1';
}

int FpcSensor::ackIrq() {
    return pwrite(mIrqFd.get(), "1", 1, 0) == 1 ? 0 : -errno;
}

FpcSensor::Wait FpcSensor::waitIrq(int cancelFd, int timeoutMs) {
    // Reading the attribute (re)arms sysfs_notify(). The line may already be high when we get here.
    if (irqAsserted()) return Wait::IRQ;
    pollfd fds[2] = {
            {mIrqFd.get(), POLLPRI | POLLERR, 0},
            {cancelFd, POLLIN, 0},
    };
    for (;;) {
        int r = poll(fds, cancelFd >= 0 ? 2 : 1, timeoutMs);
        if (r < 0) {
            if (errno == EINTR) continue;
            PLOG(ERROR) << "poll irq";
            return Wait::ERROR;
        }
        if (r == 0) return Wait::TIMEOUT;
        if (cancelFd >= 0 && (fds[1].revents & POLLIN)) return Wait::CANCELLED;
        if (fds[0].revents & (POLLPRI | POLLERR)) {
            // The edge interrupt is the event; the level is usually already low again by now.
            // Whether a finger really is down is decided by the TA (read_finger_down).
            irqAsserted();  // consume the notification
            return Wait::IRQ;
        }
    }
}

}  // namespace angler::fpc
