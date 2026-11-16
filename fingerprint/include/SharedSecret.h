/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <aidl/android/hardware/security/sharedsecret/BnSharedSecret.h>

#include <functional>
#include <mutex>
#include <vector>

namespace angler::fpc {

/**
 * ISharedSecret/fingerprint participant. keystore2 runs the HMAC key agreement across every
 * declared ISharedSecret (here: software KeyMint, software gatekeeper and us), so after
 * computeSharedSecret() we hold the same key gatekeeper signs enrol HATs with and keystore verifies
 * auth HATs with. That key is handed to the FPC TA (init_hw_auth) via onKey.
 *
 * Key derivation as in system/keymaster SoftKeymasterEnforcement::ComputeSharedHmac and
 * hardware/interfaces/gatekeeper/aidl/software/SharedSecret.cpp: CKDF(32 zero bytes,
 * "KeymasterSharedMac", seed_0 || nonce_0 || ...), check = HMAC-SHA256(key, "Keymaster HMAC
 * Verification").
 */
class SharedSecret : public aidl::android::hardware::security::sharedsecret::BnSharedSecret {
  public:
    using KeyCallback = std::function<void(std::vector<uint8_t> key)>;
    explicit SharedSecret(KeyCallback onKey) : mOnKey(std::move(onKey)) {}

    ndk::ScopedAStatus getSharedSecretParameters(
            aidl::android::hardware::security::sharedsecret::SharedSecretParameters* out) override;
    ndk::ScopedAStatus computeSharedSecret(
            const std::vector<aidl::android::hardware::security::sharedsecret::SharedSecretParameters>&
                    params,
            std::vector<uint8_t>* sharingCheck) override;

  private:
    std::mutex mLock;
    std::vector<uint8_t> mSeed;
    std::vector<uint8_t> mNonce;
    KeyCallback mOnKey;
};

}  // namespace angler::fpc
