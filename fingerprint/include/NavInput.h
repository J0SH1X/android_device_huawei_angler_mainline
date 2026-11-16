/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <android-base/unique_fd.h>

#include <cstdint>

namespace angler::fpc {

/**
 * "uinput-fpc" key device for fingerprint navigation gestures, as on stock. Only the four swipe
 * directions are emitted (KEY_UP/DOWN/LEFT/RIGHT); vendor/usr/keylayout/uinput-fpc.kl maps them to
 * SYSTEM_NAVIGATION_*. Stock also sent ENTER (tap) and ESC (hold); those are left out on purpose so a
 * casual touch on the sensor never types into an app.
 */
class NavInput {
  public:
    int open();
    /** TA gesture code (1..6) -> key press/release. */
    void report(uint32_t gesture);

  private:
    void emit(uint16_t type, uint16_t code, int32_t value);
    android::base::unique_fd mFd;
};

}  // namespace angler::fpc
