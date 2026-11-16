/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstddef>
#include <cerrno>
#include <cstdint>
#include <vector>

namespace angler::fpc {

/**
 * Transport to the fingerprint trusted application. Everything above this interface is plain
 * userspace code; everything below it needs the kernel driver. FpcTa only ever talks to this, so
 * the HAL can be exercised with a mock channel before the driver exists.
 */
class TaChannel {
  public:
    virtual ~TaChannel() = default;

    /** Loads the TA. Idempotent. Returns 0 or -errno. */
    virtual int open() = 0;
    virtual void close() = 0;

    /**
     * Copies buf[0..len) into the TA's shared buffer, runs one command, and copies the (possibly
     * modified) shared buffer back into buf. Returns 0 if the transport and the TZ-level response
     * succeeded, -errno otherwise. The TA-level result lives inside buf (see FpcProtocol.h).
     */
    virtual int transact(uint8_t* buf, size_t len) = 0;

    /** Fetches the HAT HMAC key from the secure-world keymaster. Returns 0 or -errno. */
    virtual int getAuthTokenKey(std::vector<uint8_t>* key) = 0;

    /**
     * One command to the QSEE keymaster app (which also implements gatekeeper): cmd is sent as
     * is, the response (rspCap bytes) is returned in *rsp. Returns 0 or -errno.
     */
    virtual int keymasterCall(const std::vector<uint8_t>& cmd, size_t rspCap,
                              std::vector<uint8_t>* rsp) {
        (void)cmd, (void)rspCap, (void)rsp;
        return -ENOSYS;
    }
};

}  // namespace angler::fpc
