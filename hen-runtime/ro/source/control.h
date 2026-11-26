// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <switch/types.h>
#include <switch/result.h>

// Experimental extension; must NOT be exposed to ordinary NRO clients.
// Every command runs on RO's single-threaded service dispatcher.
Result roControlAdopt(Handle copied_session, u64 sender_pid, u64* out_hen_pid);
Result roControlAuthorizeSupervisor(u64 supervisor_pid, u64 sender_pid);
Result roControlStage(Handle copied_session, u64 sender_pid, u64 generation);
Result roControlSwitch(u64 sender_pid, u64 generation);
Result roControlRollback(u64 sender_pid, u64 generation);
Result roControlFinalize(u64 sender_pid, u64 generation);
Result roControlCreateHomebrewSession(Handle process, Handle* out_client);
bool roControlIsSupervisor(u64 sender_pid);
