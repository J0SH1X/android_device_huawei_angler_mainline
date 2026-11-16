/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "Hat.h"

#include <endian.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>

#include <cstring>

namespace angler::fpc {

using aidl::android::hardware::keymaster::HardwareAuthenticatorType;
using aidl::android::hardware::keymaster::HardwareAuthToken;

namespace {
constexpr size_t kMacLen = 32;
constexpr size_t kOffChallenge = 1, kOffUser = 9, kOffAuthId = 17, kOffType = 25, kOffTime = 29,
                 kOffMac = 37;
static_assert(kOffMac + kMacLen == kHatSize, "hw_auth_token_t layout");
}  // namespace

bool hatToBytes(const HardwareAuthToken& hat, HatBytes* out) {
    if (hat.mac.size() != kMacLen) return false;
    out->fill(0);
    uint8_t* b = out->data();
    b[0] = 0;
    memcpy(b + kOffChallenge, &hat.challenge, 8);
    memcpy(b + kOffUser, &hat.userId, 8);
    memcpy(b + kOffAuthId, &hat.authenticatorId, 8);
    uint32_t type = htobe32(static_cast<uint32_t>(hat.authenticatorType));
    memcpy(b + kOffType, &type, 4);
    uint64_t ts = htobe64(static_cast<uint64_t>(hat.timestamp.milliSeconds));
    memcpy(b + kOffTime, &ts, 8);
    memcpy(b + kOffMac, hat.mac.data(), kMacLen);
    return true;
}

bool hatFromBytes(const HatBytes& in, HardwareAuthToken* out) {
    const uint8_t* b = in.data();
    if (b[0] != 0) return false;
    memcpy(&out->challenge, b + kOffChallenge, 8);
    memcpy(&out->userId, b + kOffUser, 8);
    memcpy(&out->authenticatorId, b + kOffAuthId, 8);
    uint32_t type;
    memcpy(&type, b + kOffType, 4);
    out->authenticatorType = static_cast<HardwareAuthenticatorType>(be32toh(type));
    uint64_t ts;
    memcpy(&ts, b + kOffTime, 8);
    out->timestamp.milliSeconds = static_cast<int64_t>(be64toh(ts));
    out->mac.assign(b + kOffMac, b + kOffMac + kMacLen);
    return true;
}

std::vector<uint8_t> hatMac(const HardwareAuthToken& hat, const std::vector<uint8_t>& key) {
    HardwareAuthToken t = hat;
    t.mac.assign(kMacLen, 0);
    HatBytes bytes;
    if (key.empty() || !hatToBytes(t, &bytes)) return {};
    std::vector<uint8_t> mac(kMacLen);
    unsigned int len = 0;
    if (!HMAC(EVP_sha256(), key.data(), key.size(), bytes.data(), kOffMac, mac.data(), &len) ||
        len != kMacLen)
        return {};
    return mac;
}

bool hatVerify(const HardwareAuthToken& hat, const std::vector<uint8_t>& key) {
    std::vector<uint8_t> mac = hatMac(hat, key);
    return !mac.empty() && hat.mac.size() == kMacLen &&
           CRYPTO_memcmp(mac.data(), hat.mac.data(), kMacLen) == 0;
}

}  // namespace angler::fpc
