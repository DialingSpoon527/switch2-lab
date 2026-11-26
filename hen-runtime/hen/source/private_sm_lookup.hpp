// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <switch.h>

namespace pluginmanager {
enum MigrationLookupStep : u32 {
    LookupConnect = 0x20001,
    LookupRegister = 0x20002,
    LookupService = 0x20003,
};

// Use the per-plugin proxy already created by HEN. It delegates service access
// through FS's initialized SM session, just like ordinary homebrew. A fresh
// connection registered as HEN has different access rights. Do not register or
// detach this delegated session, and never close its borrowed client here.
// The proxy protects the master from malformed messages, but all proxy lookups
// share that master: a lookup that never completes can still stall other lookups.
inline Result lookupMigrationOnDelegatedSm(Handle sm_handle, Handle* out, u32* failed_step) {
    if (!out || !failed_step || sm_handle == INVALID_HANDLE)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *out = INVALID_HANDLE;
    *failed_step = 0;
    Service sm = {};
    serviceCreate(&sm, sm_handle);
    sm.own_handle = false;
    const SmServiceName name = smEncodeName("mig:usr");
    Result rc = serviceDispatchIn(&sm, 1, name,
        .out_handle_attrs = {SfOutHandleAttr_HipcMove}, .out_handles = out);
    if (R_SUCCEEDED(rc) && *out == INVALID_HANDLE)
        rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (R_FAILED(rc)) *failed_step = LookupService;
    return rc;
}
} // namespace pluginmanager
