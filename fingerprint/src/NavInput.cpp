/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "fpc-angler-nav"

#include "NavInput.h"

#include <android-base/logging.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace angler::fpc {

namespace {
// Stock default table (gesture 1..6): ENTER, ESC, UP, DOWN, LEFT, RIGHT. 0 = not emitted.
constexpr uint16_t kKeys[7] = {0, 0, 0, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT};
}  // namespace

int NavInput::open() {
    mFd.reset(::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC));
    if (!mFd.ok()) {
        PLOG(ERROR) << "open /dev/uinput";
        return -errno;
    }
    if (ioctl(mFd.get(), UI_SET_EVBIT, EV_KEY) < 0) return -errno;
    for (uint16_t k : kKeys) {
        if (k && ioctl(mFd.get(), UI_SET_KEYBIT, k) < 0) return -errno;
    }
    uinput_setup setup = {};
    strlcpy(setup.name, "uinput-fpc", sizeof(setup.name));
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0;
    setup.id.product = 1;
    if (ioctl(mFd.get(), UI_DEV_SETUP, &setup) < 0 || ioctl(mFd.get(), UI_DEV_CREATE) < 0) {
        PLOG(ERROR) << "create uinput-fpc";
        mFd.reset();
        return -errno;
    }
    LOG(INFO) << "uinput-fpc created";
    return 0;
}

void NavInput::emit(uint16_t type, uint16_t code, int32_t value) {
    input_event ev = {};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    (void)!write(mFd.get(), &ev, sizeof(ev));
}

void NavInput::report(uint32_t gesture) {
    if (!mFd.ok() || gesture >= 7 || !kKeys[gesture]) return;
    LOG(INFO) << "gesture " << gesture << " -> key " << kKeys[gesture];
    emit(EV_KEY, kKeys[gesture], 1);
    emit(EV_SYN, SYN_REPORT, 0);
    emit(EV_KEY, kKeys[gesture], 0);
    emit(EV_SYN, SYN_REPORT, 0);
}

}  // namespace angler::fpc
