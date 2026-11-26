// SPDX-License-Identifier: GPL-2.0-only
#include <switch.h>
#include <new>
#include "server.hpp"

extern "C" {
u32 __nx_applet_type = AppletType_None;
size_t __nx_heap_size = 0x200000;

void __appInit(void) {
}

void __appExit(void) {
}
}

int main(int argc, char** argv) {
    NX_IGNORE_ARG(argc);
    NX_IGNORE_ARG(argv);
    Result rc = smInitialize();
    if (R_FAILED(rc)) {
        return static_cast<int>(rc);
    }
    {
        // Keep the enlarged manager off the signed title's main stack.
        // Destroy it before smExit so its IPC dependencies remain available.
        HenServer* server = new (std::nothrow) HenServer;
        if (!server) {
            rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        } else {
            rc = server->initialize();
            if (R_SUCCEEDED(rc)) rc = server->run();
            delete server;
        }
    }
    smExit();
    return static_cast<int>(rc);
}
