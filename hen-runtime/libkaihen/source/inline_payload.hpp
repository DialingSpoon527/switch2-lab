// SPDX-License-Identifier: ISC
// Decode libounce's non-domain, inline-only CMIF scalar payload length.
#pragma once
#include "cmif_server.hpp"

namespace ounce {

// CMIF's ordinary request encoder reserves 16 bytes for alignment BEFORE
// the CMIF header. parseRequest() can therefore expose up to 16 bytes of
// trailing padding after the real payload. Pass the normalized length to
// scalar handlers; otherwise e.g. a u32 ping is seen as a 12-byte request.
//
// The wire carries a word-rounded length, not the original byte count.
// Variable-length protocols must carry their own explicit length. Handles
// and the optional PID are allowed here and must be validated by the caller.
// This helper does not accept domain messages or mapped/pointer buffers.
inline bool inlinePayloadSize(const CmifRequest& request, size_t* size) {
    if (!size) return false;
    *size = 0;
    const HipcMetadata& meta = request.hipc.meta;
    if (!request.valid ||
        (meta.type != CmifCommandType_Request &&
         meta.type != CmifCommandType_RequestWithContext) ||
        meta.num_send_statics || meta.num_recv_statics ||
        meta.num_send_buffers || meta.num_recv_buffers || meta.num_exch_buffers)
        return false;
    constexpr size_t overhead = 16 + sizeof(CmifInHeader);
    if (meta.num_data_words < overhead / sizeof(u32) ||
        meta.num_data_words > MessageSize / sizeof(u32))
        return false;
    const size_t payload = static_cast<size_t>(meta.num_data_words) * sizeof(u32) - overhead;
    if (payload > request.data_size || request.data_size - payload > 16)
        return false;
    *size = payload;
    return true;
}

} // namespace ounce
