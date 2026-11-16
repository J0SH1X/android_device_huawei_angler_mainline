/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace angler::fpc {

/**
 * REE side of QSEE listener 0xa ("file system"). The TA asks the normal world to open/read/write
 * files; the TA owns the template db format, we only move bytes.
 *
 * Layouts follow protocol-notes.md; lines marked ASSUMED in FsListener.cpp are not fully pinned
 * down there.
 *
 * Differences from the stock listener (deliberate):
 *  - every path must be absolute, free of ".." and inside a configured root; writes only inside
 *    the writable root
 *  - fds handed to the TA are tracked; operations on any other fd are refused
 *  - no system(): chown/chmod (0x214) is refused
 *
 * Pure function of (request bytes) -> (response bytes); the transport in QseecomLiteChannel only
 * shuttles buffers, so this is unit-testable on the host.
 */
class FsListener {
  public:
    static constexpr uint32_t kListenerId = 0xa;
    static constexpr size_t kReqBufSize = 0x4e48;
    static constexpr size_t kRspBufSize = 0x4e48;
    static constexpr size_t kMaxIo = 20000;
    static constexpr size_t kPathLen = 0x100;

    FsListener(std::string writableRoot, std::vector<std::string> readOnlyRoots = {});
    ~FsListener();

    /** Handles one request. rsp must hold kRspBufSize bytes; *rspLen is the number to send back. */
    void handle(const uint8_t* req, size_t reqLen, uint8_t* rsp, size_t* rspLen);

    /**
     * QSEE listener 0x7000, "GP file system" (the FPC TA keeps its template db here), in place:
     *   req: u32 op, char path[0x100], u32 offset @0x104, u32 size @0x108, data @0x10c
     *   op 0 read   -> u32 op, i32 status, u32 bytes, data @0xc
     *   op 1 write  -> u32 op, i32 status, u32 bytes      (offset 0 truncates)
     *   op 2 remove -> u32 op, i32 status
     * Same path confinement as handle().
     */
    static constexpr uint32_t kGpfsListenerId = 0x7000;
    static constexpr size_t kGpfsBufSize = 0x7e000;
    void handleGpfs(uint8_t* buf, size_t len, size_t* rspLen);

  private:
    bool resolve(const uint8_t* req, size_t reqLen, size_t off, bool write, std::string* out);
    bool tracked(int fd) const { return mFds.count(fd) != 0; }

    std::string mWritableRoot;
    std::vector<std::string> mReadOnlyRoots;
    std::set<int> mFds;
    int mLastErrno = 0;
};

}  // namespace angler::fpc
