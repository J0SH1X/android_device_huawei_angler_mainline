/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <aidl/android/hardware/keymaster/HardwareAuthToken.h>

#include <vector>

#include "FpcTa.h"

namespace angler::fpc {

/**
 * hw_auth_token_t, packed (hardware/hw_auth_token.h):
 *   u8 version(0) | u64 challenge | u64 user_id | u64 authenticator_id (native endian)
 *   | u32 authenticator_type | u64 timestamp (network byte order) | u8 hmac[32]
 */
bool hatToBytes(const aidl::android::hardware::keymaster::HardwareAuthToken& hat, HatBytes* out);
bool hatFromBytes(const HatBytes& in, aidl::android::hardware::keymaster::HardwareAuthToken* out);

/** HMAC-SHA256 over the token without its mac, as hw_auth_token_t is signed. */
std::vector<uint8_t> hatMac(const aidl::android::hardware::keymaster::HardwareAuthToken& hat,
                            const std::vector<uint8_t>& key);
bool hatVerify(const aidl::android::hardware::keymaster::HardwareAuthToken& hat,
               const std::vector<uint8_t>& key);

}  // namespace angler::fpc
