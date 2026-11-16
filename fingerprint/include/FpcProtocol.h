/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wire protocol of the FPC "fingerpr" QSEE trusted application, as recovered in
 * fingerprint-re/protocol-notes.md. Nothing in here is copied from vendor code; it is the
 * command/offset table only.
 *
 * Items marked ASSUMED are not pinned down by the notes and must be confirmed against real
 * hardware once the kernel side exists. They are all collected here so there is one place to fix.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace angler::fpc {

// ---- command block header (start of the shared buffer) ------------------------------------------
constexpr size_t kOffGroup = 0x00;
constexpr size_t kOffCmd = 0x04;
constexpr size_t kOffResult = 0x08;  // i32; for some commands an input (set_gid)
constexpr size_t kOffOut = 0x0c;     // u32 out (or input length for group 2/3)
constexpr size_t kOffPayload = 0x10;

constexpr uint32_t kGroupMain = 1;
constexpr uint32_t kGroupDb = 2;
constexpr uint32_t kGroupAuth = 3;
constexpr uint32_t kGroupNav = 8;

// Group 8 (navigation gestures, RE: stock fpc_tee_nav_*): 1 start, 2 stop, 3 poll
// (+0xc u32 gesture 1..6, +0x10 u8 finger present), 4 arm finger detect (+0xc = 0, then irq),
// 5 set config, 6 get config. Gestures: 1 tap, 2 hold, 3 up, 4 down, 5 left, 6 right.
constexpr uint32_t kCmdNavStart = 1;
constexpr uint32_t kCmdNavStop = 2;
constexpr uint32_t kCmdNavPoll = 3;
constexpr uint32_t kCmdNavArm = 4;

// Group 1
constexpr uint32_t kCmdBeginEnrol = 0x00;
constexpr uint32_t kCmdEnrol = 0x01;
constexpr uint32_t kCmdEndEnrol = 0x02;
constexpr uint32_t kCmdIdentify = 0x03;
constexpr uint32_t kCmdUpdateTemplate = 0x04;
constexpr uint32_t kCmdFingerLost = 0x05;
constexpr uint32_t kCmdArmFingerDown = 0x07;
constexpr uint32_t kCmdReadFingerDown = 0x08;
constexpr uint32_t kCmdLoadEmptyDb = 0x0a;
constexpr uint32_t kCmdGetFingerIds = 0x0d;
constexpr uint32_t kCmdDeleteTemplate = 0x0e;
constexpr uint32_t kCmdCapture = 0x0f;
constexpr uint32_t kCmdSetGid = 0x10;
constexpr uint32_t kCmdDbId = 0x11;
constexpr uint32_t kCmdSensorIdle = 0x12;  // also "init" / "release"
constexpr uint32_t kCmdOtpInfo = 0x14;

// Group 2 (template db, the TA reads/writes the file through the REE FS listener)
constexpr uint32_t kCmdDbLoad = 0x0b;
constexpr uint32_t kCmdDbStore = 0x0c;

// Group 3 (hardware auth)
constexpr uint32_t kCmdSetAuthChallenge = 1;
constexpr uint32_t kCmdGetEnrolChallenge = 2;
constexpr uint32_t kCmdAuthorizeEnrol = 3;
constexpr uint32_t kCmdGetAuthResult = 4;
constexpr uint32_t kCmdInitHwAuth = 5;

// ---- shared buffer sizes -------------------------------------------------------------------------
constexpr size_t kSharedDefault = 0x48;     // init/release/most commands
constexpr size_t kSharedOtp = 0x44;
constexpr size_t kSharedAuthToken = 0x55;   // group 3 cmd 3
constexpr size_t kHatSize = 69;             // hw_auth_token_t, packed
constexpr size_t kHatKeyMax = 0x100;        // ASSUMED upper bound for the HMAC key (32 in practice)
constexpr size_t kMaxFingers = 5;           // get_finger_ids count in must be < 6

// ---- ASSUMED semantics ---------------------------------------------------------------------------
// Header result (+8) of 0 means success for every command.
constexpr int32_t kTaOk = 0;
// Group 2 result meaning "template db file is corrupt" (notes: -6) -> fall back to an empty db.
constexpr int32_t kTaDbCorrupt = -6;
constexpr int32_t kTaErrTimeout = -2;
constexpr int32_t kTaErrNoSpace = -5;
constexpr int32_t kTaErrCancelled = -7;

enum EnrolStatus : int32_t {
    kEnrolDone = 0,
    kEnrolGood = 1,
    kEnrolAbort = 2,
    kEnrolAbortVendor = 3,
    kEnrolPartial = 4,
    kEnrolVendor = 5,
    kEnrolDirty = 6,
};

enum CaptureResult : int32_t {
    kCaptureOk = 0,
    kCaptureTooFast = 1,
    kCaptureInsufficient = 2,
};
constexpr uint8_t kCaptureFlagEnrol = 0;
constexpr uint8_t kCaptureFlagAuth = 1;
// ---- result semantics, as used by the stock HAL fingerprint.angler.so (do_enroll/do_authenticate)
// identify: status (+8) != 0 is an error; fid (+0xc) != 0 is a match, 0 a rejection.
// enrol: status (+8) < 0 error; 0 enrollment complete (-> end_enrol); 1 good sample; 2 abort
//        (UNABLE_TO_PROCESS); 3 abort (vendor); 4 partial; 5 vendor acquired; 6 sensor dirty;
//        others: progress only. Remaining samples at +0xc.
// update_template: 0 means the template set changed, persist the db.
// capture result: 0 ok, 1 too fast, 2 insufficient, -7 cancelled, other != 0 error.
// errors: -2 timeout, -5 no space, -7 cancelled.

// ---- capture path, as in stock fpc_tee_capture_image (RE, lib_fpc_tac_shared.so) ---------------
// finger_lost (0x05, polled every 20 ms): > 0 lifted, 0 still down, < 0 error.
// arm_finger_down (0x07): 0 armed (wait for irq), anything else aborts the capture.
// read_finger_down (0x08, after irq): > 0 finger down, 0 spurious (re-arm), < 0 error.
// capture (0x0f): u8 flag at +0xc, result 0 = image ok. Always followed by wakeup disable + 0x12.

}  // namespace angler::fpc
