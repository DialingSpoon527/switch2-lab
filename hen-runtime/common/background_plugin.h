// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <switch.h>

// An internal HEN control handoff from upstream bootstrap. This is a COPY
// of an already acquired pm:shell session, not a new privilege escalation.
struct BackgroundPluginCapability {
    u64 program_id;
    u32 flags;
    u8 storage_id;
    u8 reserved[3];
};
#ifdef __cplusplus
static_assert(sizeof(BackgroundPluginCapability) == 16, "background handoff layout");
#else
_Static_assert(sizeof(struct BackgroundPluginCapability) == 16, "background handoff layout");
#endif

