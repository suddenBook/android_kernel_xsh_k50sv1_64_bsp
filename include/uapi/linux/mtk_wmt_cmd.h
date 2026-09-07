/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_MTK_WMT_CMD_H
#define _UAPI_LINUX_MTK_WMT_CMD_H

#include <linux/types.h>
#include <linux/ioctl.h>

/* Multibyte fields are little-endian; native and compat layouts are identical. */
#define WMT_CMD2_MAGIC 0x32544d57U /* W M T 2 in byte order */
#define WMT_CMD2_VERSION 2
#define WMT_CMD2_COMMAND_MAX 255
#define WMT_CMD2_PATCH_MAX 10
#define WMT_CMD2_ROM_MAX 5
#define WMT_CMD2_FRAME_HEADER_SIZE 32
#define WMT_CMD2_RECORD_SIZE 264
#define WMT_CMD2_READ_MAX (32 + WMT_CMD2_COMMAND_MAX)
#define WMT_CMD2_WRITE_MAX (32 + 8 + WMT_CMD2_PATCH_MAX * WMT_CMD2_RECORD_SIZE)

enum wmt_cmd2_kind {
	WMT_CMD2_COMMAND = 1,
	WMT_CMD2_STATUS = 2,
	WMT_CMD2_PATCH_LIST = 3,
	WMT_CMD2_ROM_LIST = 4,
};

struct wmt_cmd2_header {
	__u32 magic;
	__u16 version;
	__u16 kind;
	__aligned_u64 session_id;
	__aligned_u64 transaction_id;
	__u32 payload_len;
	__s32 result;
};

struct wmt_cmd2_record {
	__u32 index; /* Patch sequence 1..count, or ROM type 0..4. */
	__u8 address[4];
	__u8 name[256]; /* Nonempty, NUL-terminated filename. */
};

struct wmt_cmd2_list {
	__u32 count;
	__u32 reserved; /* Must be zero. */
	struct wmt_cmd2_record record[];
};

enum wmt_cmd2_session_action {
	WMT_CMD2_BIND = 1,
	WMT_CMD2_UNBIND = 2,
};

struct wmt_cmd2_session {
	__u32 version;
	__u32 action;
	__aligned_u64 session_id; /* BIND input: zero. UNBIND input: current ID. */
	__u32 max_read_bytes; /* Input: zero. Output: WMT_CMD2_READ_MAX. */
	__u32 max_write_bytes; /* Input: zero. Output: WMT_CMD2_WRITE_MAX. */
	__u32 flags; /* Input and output: zero. */
	__u32 reserved; /* Input and output: zero. */
};

/* One command owner per open file description; other opens remain control-only. */
#define WMT_IOCTL_CMD2_SESSION _IOWR(0xa0, 64, struct wmt_cmd2_session)

#endif /* _UAPI_LINUX_MTK_WMT_CMD_H */
