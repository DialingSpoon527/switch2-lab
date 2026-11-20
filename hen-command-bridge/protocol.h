/* Local kaiHEN command bridge wire format. All integers are little-endian. */
#ifndef HCB_PROTOCOL_H
#define HCB_PROTOCOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HCB_MAGIC UINT32_C(0x31424348)
#define HCB_VERSION 2
#define HCB_HEADER_SIZE 24
#define HCB_MAX_PACKET (1024 * 1024)
#define HCB_MAX_BUFFER (256 * 1024)
#define HCB_MAX_INLINE 128
#define HCB_MAX_ITEMS 4
#define HCB_MAX_BULK (8 * 1024 * 1024)
#define HCB_BAD_REQUEST UINT32_C(0xf0010001)
#define HCB_BAD_RESOURCE UINT32_C(0xf0010003)
#define HCB_LIMIT UINT32_C(0xf0010004)
#define HCB_BUSY UINT32_C(0xf0010005)
#define HCB_BAD_RANGE UINT32_C(0xf0010006)
#define HCB_BAD_REPLY UINT32_C(0xf0010007)
#define HCB_IDENTITY UINT32_C(0xf0010008)
#define HCB_INTERFACE UINT32_C(0xf0010009)

enum HcbOperation {
    Hcb_Status = 2, Hcb_GetInfo = 3,
    Hcb_QueryMemory = 4, Hcb_ReadMemory = 5, Hcb_Allocate = 6,
    Hcb_WriteAllocation = 7, Hcb_Close = 8, Hcb_Resources = 9,
    Hcb_OpenService = 10, Hcb_CloneService = 11, Hcb_Domain = 12,
    Hcb_Ipc = 13, Hcb_HandleToService = 14, Hcb_HenResource = 15,
    Hcb_Jit = 16, Hcb_Call = 17, Hcb_Symbols = 18, Hcb_SyscallHints = 19,
    Hcb_ReadBulk = 20, Hcb_ReadInfo = 21, Hcb_ReadMemoryBulk = 22, Hcb_NetworkRead = 23
};
enum HcbInterface { Hcb_Unknown=0, Hcb_FspRoot=1, Hcb_FileSystem=2, Hcb_ReadFile=3, Hcb_Storage=4 };

typedef struct { const uint8_t *data; size_t size, at; } HcbReader;
typedef struct {
    uint32_t resource, command, context, flags, input_size, output_size;
    uint32_t buffer_count, input_handle_count, output_handle_count;
    uint32_t input_object_count, output_object_count;
    uint32_t buffer_attrs[HCB_MAX_ITEMS], buffer_sizes[HCB_MAX_ITEMS];
    uint32_t input_handles[HCB_MAX_ITEMS], output_handle_attrs[HCB_MAX_ITEMS];
    uint32_t input_objects[HCB_MAX_ITEMS];
    const uint8_t *input, *buffers[HCB_MAX_ITEMS];
} HcbIpc;

static inline uint32_t hcb_u32(const void *data) {
    const uint8_t *p = data;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint64_t hcb_u64(const void *data) {
    const uint8_t *p = data;
    return hcb_u32(p) | (uint64_t)hcb_u32(p + 4) << 32;
}
static inline uint32_t hcb_opened_interface(uint32_t parent, uint32_t command, const uint8_t *input, size_t size, uint32_t objects) {
    if (objects!=1) return Hcb_Unknown;
    if (parent==Hcb_FspRoot) {
        if ((command==18 && !size) || (command==11 && size==4) || ((command==51 || command==53) && size==72)) return Hcb_FileSystem;
        if ((command==12 && size==4) || ((command==200 || command==203) && !size) ||
            (command==201 && size==8) || (command==202 && size==16)) return Hcb_Storage;
    }
    if (parent==Hcb_FileSystem && command==8 && size==4 && input && hcb_u32(input)==1) return Hcb_ReadFile;
    return Hcb_Unknown;
}
static inline bool hcb_bulk_range(uint64_t offset, uint32_t length, uint64_t size) {
    return length>0 && length<=HCB_MAX_BULK && size<=INT64_MAX && offset<=size && offset<=INT64_MAX;
}
static inline void hcb_put32(void *data, uint32_t value) {
    uint8_t *p = data;
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (i * 8));
}
static inline void hcb_put64(void *data, uint64_t value) {
    hcb_put32(data, (uint32_t)value);
    hcb_put32((uint8_t *)data + 4, (uint32_t)(value >> 32));
}
static inline const uint8_t *hcb_take(HcbReader *r, size_t size) {
    if (size > r->size - r->at) return NULL;
    const uint8_t *p = r->data + r->at;
    r->at += size;
    return p;
}
static inline bool hcb_valid_header(const uint8_t *p) {
    return hcb_u32(p) == HCB_MAGIC && p[4] == HCB_VERSION && p[5] == 0 &&
           hcb_u32(p + 12) <= HCB_MAX_PACKET &&
           hcb_u32(p + 16) == 0 && hcb_u32(p + 20) == 0;
}
static inline bool hcb_buffer_attr_valid(uint32_t attr, uint32_t size) {
    const uint32_t direction = attr & 3, transport = attr & (4 | 8 | 32);
    if (!direction || (attr & ~255u) || !size || size > HCB_MAX_BUFFER) return false;
    if (transport != 4 && transport != 8 && transport != 32) return false;
    if ((attr & 16) && (transport != 8 || !(direction & 2))) return false;
    if ((attr & (64 | 128)) && transport == 8) return false;
    if ((attr & (64 | 128)) == (64 | 128)) return false;
    return transport != 8 || size <= 65535;
}
static inline bool hcb_parse_ipc(const uint8_t *data, size_t size, HcbIpc *out) {
    if (size < 48) return false;
    memset(out, 0, sizeof(*out));
    out->resource = hcb_u32(data);
    out->command = hcb_u32(data + 4);
    out->context = hcb_u32(data + 8);
    out->flags = hcb_u32(data + 12);
    out->input_size = hcb_u32(data + 16);
    out->output_size = hcb_u32(data + 20);
    out->buffer_count = hcb_u32(data + 24);
    out->input_handle_count = hcb_u32(data + 28);
    out->output_handle_count = hcb_u32(data + 32);
    out->input_object_count = hcb_u32(data + 36);
    out->output_object_count = hcb_u32(data + 40);
    if (hcb_u32(data + 44) || (out->flags & ~1u) ||
        out->input_size > HCB_MAX_INLINE || out->output_size > HCB_MAX_INLINE ||
        out->buffer_count > HCB_MAX_ITEMS || out->input_handle_count > HCB_MAX_ITEMS ||
        out->output_handle_count > HCB_MAX_ITEMS || out->input_object_count > HCB_MAX_ITEMS ||
        out->output_object_count > HCB_MAX_ITEMS) return false;
    HcbReader r = {data, size, 48};
    for (uint32_t i = 0; i < out->buffer_count; i++) {
        const uint8_t *p = hcb_take(&r, 8);
        if (!p) return false;
        out->buffer_attrs[i] = hcb_u32(p);
        out->buffer_sizes[i] = hcb_u32(p + 4);
        if (!hcb_buffer_attr_valid(out->buffer_attrs[i], out->buffer_sizes[i])) return false;
    }
    for (uint32_t i = 0; i < out->input_handle_count; i++) {
        const uint8_t *p = hcb_take(&r, 4);
        if (!p) return false;
        out->input_handles[i] = hcb_u32(p);
    }
    for (uint32_t i = 0; i < out->output_handle_count; i++) {
        const uint8_t *p = hcb_take(&r, 4);
        if (!p) return false;
        out->output_handle_attrs[i] = hcb_u32(p);
        if (out->output_handle_attrs[i] != 1 && out->output_handle_attrs[i] != 2) return false;
    }
    for (uint32_t i = 0; i < out->input_object_count; i++) {
        const uint8_t *p = hcb_take(&r, 4);
        if (!p) return false;
        out->input_objects[i] = hcb_u32(p);
    }
    out->input = hcb_take(&r, out->input_size);
    if (!out->input) return false;
    for (uint32_t i = 0; i < out->buffer_count; i++) {
        out->buffers[i] = hcb_take(&r, out->buffer_sizes[i]);
        if (!out->buffers[i]) return false;
    }
    return r.at == r.size;
}

static inline bool hcb_ipc_shape_valid(const HcbIpc *p, bool domain) {
    if (p->input_object_count && !domain) return false;
    if (domain && p->input_object_count && p->input_size % 4) return false;
    if (domain && p->output_object_count && p->output_size % 4) return false;
    return true;
}

static inline bool hcb_ipc_pointer_budget_valid(const HcbIpc *p, size_t remaining) {
    for (uint32_t i = 0; i < p->buffer_count; i++) {
        uint32_t attr = p->buffer_attrs[i];
        if (!(attr & (8 | 32))) continue;
        for (unsigned direction = 1; direction <= 2; direction <<= 1) {
            if (!(attr & direction)) continue;
            if ((attr & 8) && p->buffer_sizes[i] > remaining) return false;
            if (p->buffer_sizes[i] <= remaining) remaining -= p->buffer_sizes[i];
        }
    }
    return true;
}

/* Matches libounce's CMIF layout, checked before it writes into TLS. */
static inline size_t hcb_ipc_tls_size(const HcbIpc *p, bool domain) {
    size_t send_statics = 0, map_buffers = 0, recv_statics = 0, pointer_sizes = 0;
    for (uint32_t i = 0; i < p->buffer_count; i++) {
        uint32_t attr = p->buffer_attrs[i];
        if (attr & 32) {
            if (attr & 1) { send_statics++; map_buffers++; }
            if (attr & 2) { recv_statics++; pointer_sizes++; map_buffers++; }
        } else if (attr & 8) {
            if (attr & 1) send_statics++;
            if (attr & 2) { recv_statics++; if (!(attr & 16)) pointer_sizes++; }
        } else map_buffers++;
    }
    size_t raw = 16 + 16 + p->input_size;
    if (domain) raw += 16 + 4 * p->input_object_count;
    raw = ((raw + 1) & ~(size_t)1) + 2 * pointer_sizes;
    raw = (raw + 3) & ~(size_t)3;
    size_t size = 8 + 8 * send_statics + 12 * map_buffers + raw + 8 * recv_statics;
    if (p->input_handle_count || (p->flags & 1)) {
        size += 4 + 4 * p->input_handle_count + ((p->flags & 1) ? 8 : 0);
    }
    return size;
}
#endif
