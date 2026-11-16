/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * qseecom-lite: minimal userspace access to QSEE trusted applications on
 * Qualcomm SoCs whose bootloader already set up the secure app region
 * (msm8994 "qcom,appsbl-qseecom-support").
 *
 * One open file = one client. Per file there is at most one shared buffer
 * (ALLOC_SHM, then mmap() the device fd at offset 0) and any number of
 * listeners.
 *
 *  LOAD_APP     looks the app up by name in QSEE; if it is not loaded yet the
 *               kernel loads <name>.mdt (+.bNN) or <name>.mbn via
 *               request_firmware(). Apps are never unloaded (other clients,
 *               e.g. the bootloader's keymaster, may share them).
 *  SEND_CMD     cmd/rsp are bounced through TZ-visible memory. With
 *               QSL_SEND_USE_SHM the physical address of the file's shared
 *               buffer is written as u32 to cmd[shm_cmd_off] first. Returns 0
 *               when QSEE reported success; listener requests issued by the
 *               app meanwhile are routed to the registered listener.
 *  LISTENER_*   REGISTER, then loop WAIT (receive the request, i.e. the whole
 *               listener buffer) / RESPOND (response is written back to the
 *               same buffer). Unregistered on close().
 */
#ifndef _UAPI_LINUX_QSEECOM_LITE_H
#define _UAPI_LINUX_QSEECOM_LITE_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define QSL_IOC_MAGIC		'Q'
#define QSL_NAME_LEN		32
#define QSL_MAX_BUF		(64 * 1024)	/* cmd, rsp */
#define QSL_MAX_SHM		(1024 * 1024)
#define QSL_MAX_LISTENER_BUF	(512 * 1024)

struct qsl_load_app {
	char name[QSL_NAME_LEN];	/* "fingerpr", "keymaster" */
	__u32 app_id;			/* out */
	__u32 reserved;
};

struct qsl_shm {
	__u32 len;			/* in; page aligned by the kernel */
	__u32 reserved;
};

#define QSL_SEND_USE_SHM	(1 << 0)

struct qsl_send_cmd {
	__u32 app_id;
	__u32 flags;
	__u32 shm_cmd_off;
	__u32 cmd_len;
	__u64 cmd_ptr;
	__u32 rsp_len;
	__u32 reserved;
	__u64 rsp_ptr;
};

struct qsl_listener_reg {
	__u32 id;
	__u32 buf_len;
};

struct qsl_listener_io {
	__u32 id;
	__u32 len;	/* WAIT: in capacity, out bytes copied; RESPOND: in length */
	__u64 buf_ptr;
};

#define QSL_IOC_LOAD_APP		_IOWR(QSL_IOC_MAGIC, 1, struct qsl_load_app)
#define QSL_IOC_UNLOAD_APP		_IOW(QSL_IOC_MAGIC, 2, __u32)	/* no-op */
#define QSL_IOC_ALLOC_SHM		_IOW(QSL_IOC_MAGIC, 3, struct qsl_shm)
#define QSL_IOC_SEND_CMD		_IOW(QSL_IOC_MAGIC, 4, struct qsl_send_cmd)
#define QSL_IOC_LISTENER_REGISTER	_IOW(QSL_IOC_MAGIC, 5, struct qsl_listener_reg)
#define QSL_IOC_LISTENER_WAIT		_IOWR(QSL_IOC_MAGIC, 6, struct qsl_listener_io)
#define QSL_IOC_LISTENER_RESPOND	_IOW(QSL_IOC_MAGIC, 7, struct qsl_listener_io)
#define QSL_IOC_LISTENER_UNREGISTER	_IOW(QSL_IOC_MAGIC, 8, __u32)

#endif /* _UAPI_LINUX_QSEECOM_LITE_H */
