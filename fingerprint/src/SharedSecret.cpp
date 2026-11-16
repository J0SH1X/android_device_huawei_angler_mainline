/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "fpc-angler-ss"

#include "SharedSecret.h"

#include <android-base/logging.h>
#include <keymaster/android_keymaster_utils.h>
#include <keymaster/km_openssl/ckdf.h>
#include <keymaster/km_openssl/hmac.h>
#include <openssl/rand.h>

#include <cstring>

namespace angler::fpc {

using aidl::android::hardware::security::sharedsecret::SharedSecretParameters;

namespace {
constexpr const char* kKeyAgreementLabel = "KeymasterSharedMac";
constexpr const char* kKeyCheckLabel = "Keymaster HMAC Verification";
constexpr size_t kKeySize = 32;

ndk::ScopedAStatus kmError(keymaster_error_t e) {
    return ndk::ScopedAStatus(AStatus_fromServiceSpecificError(static_cast<int32_t>(e)));
}
}  // namespace

ndk::ScopedAStatus SharedSecret::getSharedSecretParameters(SharedSecretParameters* out) {
    std::lock_guard<std::mutex> l(mLock);
    if (mSeed.empty()) mSeed.assign(32, 0);  // no pre-shared seed, like the software participants
    if (mNonce.empty()) {
        mNonce.assign(32, 0);
        RAND_bytes(mNonce.data(), mNonce.size());
    }
    out->seed = mSeed;
    out->nonce = mNonce;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus SharedSecret::computeSharedSecret(const std::vector<SharedSecretParameters>& params,
                                                     std::vector<uint8_t>* sharingCheck) {
    std::vector<uint8_t> key(kKeySize);
    {
        std::lock_guard<std::mutex> l(mLock);

        keymaster::KeymasterKeyBlob kak;
        if (!kak.Reset(kKeySize)) return kmError(KM_ERROR_MEMORY_ALLOCATION_FAILED);
        memset(kak.writable_data(), 0, kKeySize);
        keymaster::KeymasterBlob label(reinterpret_cast<const uint8_t*>(kKeyAgreementLabel),
                                       strlen(kKeyAgreementLabel));

        bool foundMine = false;
        std::vector<keymaster::KeymasterBlob> context;
        for (const auto& p : params) {
            context.emplace_back(p.seed.data(), p.seed.size());
            context.emplace_back(p.nonce.data(), p.nonce.size());
            foundMine = foundMine || (p.seed == mSeed && p.nonce == mNonce);
        }
        if (!foundMine) {
            LOG(ERROR) << "own shared secret parameters missing";
            return kmError(KM_ERROR_INVALID_ARGUMENT);
        }

        keymaster::KeymasterKeyBlob hmacKey;
        if (!hmacKey.Reset(kKeySize)) return kmError(KM_ERROR_MEMORY_ALLOCATION_FAILED);
        static_assert(sizeof(keymaster_blob_t) == sizeof(keymaster::KeymasterBlob));
        keymaster_error_t e = keymaster::ckdf(
                kak, label, reinterpret_cast<const keymaster_blob_t*>(context.data()),
                context.size(), &hmacKey);
        if (e != KM_ERROR_OK) {
            LOG(ERROR) << "CKDF failed: " << e;
            return kmError(e);
        }

        keymaster::HmacSha256 hmac;
        sharingCheck->assign(32, 0);
        if (!hmac.Init(hmacKey.key_material, hmacKey.key_material_size) ||
            !hmac.Sign(reinterpret_cast<const uint8_t*>(kKeyCheckLabel), strlen(kKeyCheckLabel),
                       sharingCheck->data(), sharingCheck->size())) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        }
        memcpy(key.data(), hmacKey.key_material, kKeySize);
    }
    LOG(INFO) << "shared HMAC key agreed with " << params.size() << " participants";
    if (mOnKey) mOnKey(std::move(key));
    return ndk::ScopedAStatus::ok();
}

}  // namespace angler::fpc
