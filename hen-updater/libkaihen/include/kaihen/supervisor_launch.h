// SPDX-License-Identifier: ISC
// Experimental kaiHEN supervisor bootstrap handoff (source-only).
// Presence of these resources does NOT prove process launch capability.
#pragma once
#include <switch/types.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { KaihenSupervisorLaunchGeometryVersion = 1 };

typedef struct {
    u32 version;
    u32 flags;
    s64 data_offset;
    s64 pointer_offset;
    u64 gate_server;
    u64 gate_vptr;
    u64 gate_domain;
    u64 gate_metadata;
    u64 gate_request;
    u64 gate_waiters[7];
    // These three handles are FS-process handle VALUES, not caller-local SVC handles.
    u32 gate_receive;
    u32 gate_original_wait;
    u32 gate_park;
    u32 storage_id;
    u64 mount_table;
    u64 mount_domain;
    u64 mount_root_id;
    u64 program_id;
} KaihenSupervisorLaunchGeometry;

typedef struct {
    Handle storage;
    Handle workers[7];
    Handle server_copy;
    Handle event_read;
    Handle event_write;
    // Distinct copied handles to original borrowed SM and RO connections.
    // They may share underlying state with existing peers: never send CLOSE.
    Handle sm_master;
    Handle ro_session;
} KaihenSupervisorLaunchHandles;

#ifdef __cplusplus
}
static_assert(sizeof(KaihenSupervisorLaunchGeometry) == 168);
static_assert(sizeof(KaihenSupervisorLaunchHandles) == 52);
#else
_Static_assert(sizeof(KaihenSupervisorLaunchGeometry) == 168, "geometry ABI");
_Static_assert(sizeof(KaihenSupervisorLaunchHandles) == 52, "handle ABI");
#endif
