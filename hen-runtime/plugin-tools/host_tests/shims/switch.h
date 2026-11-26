// SPDX-License-Identifier: ISC
// Host fixture only: real libounce wire declarations with a small Mutex shim.
// These declarations do not model kernel scheduling, service permissions or PM.
#pragma once
extern "C" {
#include <switch/types.h>
#include <switch/result.h>
#include <switch/sf/cmif.h>
#include <switch/sf/service.h>
#include <switch/services/sm.h>
#include <switch/kernel/thread.h>
}
