// SPDX-License-Identifier: ISC
// Host fixture stand-ins for newlib's lock storage, not an SDK replacement.
#pragma once
#include <stdint.h>
typedef uint32_t _LOCK_T;
typedef struct { uint32_t lock, owner, counter; } _LOCK_RECURSIVE_T;
