/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "FpcTa.h"
#include "TaChannel.h"

namespace angler::fpc {

/**
 * Gatekeeper commands of the QSEE keymaster app (RE: stock gatekeeper.msm8994.so). The FPC TA
 * only accepts hardware auth tokens signed with the QSEE key, while Android here runs a software
 * gatekeeper. After the HAL has verified the software gatekeeper's token itself, it obtains a
 * QSEE-signed token for the same challenge by verifying a HAL-held random secret that is enrolled
 * in the QSEE gatekeeper once per user (secret + handle in /data/vendor/fpc, system only).
 *
 *   enroll 0x1001: u32 cmd, uid, cur_handle_off, cur_handle_len, cur_pw_off, cur_pw_len,
 *                  new_pw_off, new_pw_len | data @0x20   -> rsp {status, off, len}: handle
 *   verify 0x1002: u32 cmd, uid, u64 challenge, handle_off, handle_len, pw_off, pw_len
 *                  | data @0x20                          -> rsp {status, off, len}: hw_auth_token
 *   cmd buffer (data + 0x60) & ~0x3f, response directly after it.
 */
class QseeGatekeeper {
  public:
    QseeGatekeeper(TaChannel& ch, std::string dir) : mCh(ch), mDir(std::move(dir)) {}

    /** QSEE-signed token for (user, challenge); enrolls the user's secret on first use. */
    int tokenFor(int userId, uint64_t challenge, HatBytes* out);

  private:
    int enroll(uint32_t uid, const std::vector<uint8_t>& secret, std::vector<uint8_t>* handle);
    int verify(uint32_t uid, uint64_t challenge, const std::vector<uint8_t>& handle,
               const std::vector<uint8_t>& secret, HatBytes* out);
    int loadOrCreate(int userId, bool fresh, std::vector<uint8_t>* secret,
                     std::vector<uint8_t>* handle);
    std::string path(int userId) const;

    TaChannel& mCh;
    std::string mDir;
};

}  // namespace angler::fpc
