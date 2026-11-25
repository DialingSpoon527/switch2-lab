// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <switch.h>

// Read-only candidate start/registration flight recorder. The RO service
// thread only records fixed-size in-memory events; SD IPC is isolated to a
// lower-priority best-effort worker and is NEVER a startup prerequisite.
enum class RoStartupEvent : u32 {
    MainEntered = 1, RegisterEnter = 2, RegisterReturn = 3,
    YieldEnter = 4, YieldReturn = 5, Resume = 6, Request = 7,
    AcceptFailure = 8, SessionFull = 9, SessionAccepted = 10,
    WaitError = 11, Exiting = 12,
    SmInitEnter = 13, SmInitReturn = 14, LoggerSmConnect = 15,
    LoggerSmInit = 16, LoggerFspLookup = 17, LoggerFsInit = 18
};
void roStartupTraceStart(u64 pid);
void roStartupTraceNote(RoStartupEvent event, u32 slot, Result result, u32 extra);
