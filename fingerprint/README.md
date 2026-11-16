# angler fingerprint HAL (AIDL, FPC1020 via QSEE "fingerpr" TA)

`android.hardware.biometrics.fingerprint-service.angler` implements `IFingerprint` v5 on top of the
protocol recovered in `fingerprint-re/protocol-notes.md`. No stock userspace is used.

```
 framework ──binder──► Fingerprint / Session          (src/Fingerprint.cpp, Session.cpp)
                          │ one Worker thread, callbacks never on binder threads
                          ▼
                       FpcEngine                      (enroll / authenticate / lockout / db persist)
                    ┌─────┼───────────────┐
                    ▼     ▼               ▼
              FpcSensor  FpcTa           Hat           (sysfs irq/clk/wakeup | TA command set | HAT bytes)
                          │ TaChannel
                          ▼
                 QseecomLiteChannel ◄── FsListener 0xa (REE file-system service for the TA)
                          │  /dev/qseecom-lite
                          ▼
   kernel: drivers/firmware/qcom/qcom_qseecom_lite.c   (TA load, send, listeners via QSEE SMC calls)
           drivers/misc/fpc1020_tee.c                  (power, reset, SPI clocks, irq -> sysfs)
```

## Kernel side (kernel/mainline/msm8994-mainline)

- `CONFIG_QCOM_QSEECOM_LITE=y`: `/dev/qseecom-lite`, ABI `include/uapi/linux/qseecom_lite.h`
  (copied verbatim to `include/qseecom_lite_uapi.h` here; keep them identical). Uses a new
  `qcom_scm_qsee_call()` export in `qcom_scm.c`. Relies on the bootloader having announced the
  TZ app region and loaded cmnlib (`qcom,appsbl-qseecom-support` in the stock DT), so it does
  neither. SMC convention only (angler reports "smc arm 32"). The TA is loaded with
  `request_firmware("fingerpr.mdt")` or `fingerpr.mbn` (merged ELF, what `device.mk` ships to
  `/vendor/firmware`). `keymaster` is looked up first and normally already loaded by the bootloader.
- `CONFIG_FPC1020_TEE=y`: `/sys/devices/platform/fpc1020/{clk_enable,irq,wakeup_enable,hw_reset}`.
- DT (`msm8994-huawei-angler-rev-101.dts`): `fpc1020` node (gpio 89 reset, 90 irq, 102 id,
  `pm8994_s4`, BLSP2 QUP6 SPI + BLSP2 AHB clocks) and `qseecom-lite` node. GPIO 85-88 (the SPI bus)
  stay TZ-reserved.

If `/dev/qseecom-lite` or the sensor is missing the service logs and exits (it is `oneshot`).

## Open points / ASSUMED (all documented in `include/FpcProtocol.h` or marked in the code)

- Result codes: 0 = success; identify match = status 0 and fid != 0; enrol good sample = status 0;
  update_template > 0 = template changed (db re-stored); finger_lost > 0 = lifted; the irq itself is
  the finger-down event (0x08 result is not interpreted).
- FS listener: `write` (0x208) argument layout (fd@4, n@8, data@0xc) and `statfs` (0x218) response
  are guesses; `fcntl`, `unlinkat`, `openat`, dir iteration, `chown/chmod` (0x214) are refused
  (`-ENOSYS`). Stat response is the 0x68-byte 32-bit ARM `stat64`. Watch the log for
  "unsupported fs listener command" on first bring-up.
- Keymaster response layout ({status, offset, len} at the start of the response, key at
  response + offset) is bounds-checked but unverified.
  On a mainline userspace the keystore HMAC key may not be the one the QSEE keymaster holds; if HATs
  are rejected by keystore that is the first suspect.
- `invalidateAuthenticatorId` cannot force a new id (no TA command known); it reports the current one.
- Cancelling an enrol after some samples leaves the TA in enrol state; the next `begin_enrol` is
  expected to reset it.
- Lockout is HAL-side (5 failures: 30 s, 20: permanent until `resetLockout`).

## Storage

Template db per user: `/data/vendor/fpc/user_<id>.db` (written by the TA through the FS listener as
`*.tmp`, then renamed). The FS listener only touches that directory (read-write) and
`/odm/firmware`, `/vendor/firmware` (read-only), refuses `..`, relative paths, and fds it did not hand out.

## Tests

`m angler_fpc_proto_test` (host): TA command layouts against a mock channel, and the FS listener
against a temp dir. Run `out/host/linux-x86/nativetest64/angler_fpc_proto_test/angler_fpc_proto_test`.
