#include <switch.h>
#include <kaihen.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "protocol.h"

#define BRIDGE_PORT 8090
#define RESOURCE_LIMIT 64
#define ALLOCATION_BUDGET (16 * 1024 * 1024)
enum ResourceKind { Resource_Service = 1, Resource_Handle = 2, Resource_Buffer = 3, Resource_Jit = 4 };
typedef struct {
    u32 id, kind, parent;
    bool protected_identity;
    Service service;
    Handle handle;
    void *buffer;
    size_t size;
    Jit jit;
} Resource;

static Resource resources[RESOURCE_LIMIT];
static u32 next_id;
static size_t allocated;
static bool running = true;
static PadState pad;
static u8 request_data[HCB_MAX_PACKET];
typedef struct { u8 header[HCB_HEADER_SIZE], payload[HCB_MAX_PACKET]; } ResponseFrame;
_Static_assert(offsetof(ResponseFrame, payload) == HCB_HEADER_SIZE, "Contiguous response frame");
static ResponseFrame response_frame;
#define response_data response_frame.payload
static HenStatus hen_status;
static Result hen_result;
static u64 requests_completed;
static u64 last_ui_tick;

static bool update_ui(void) {
    // Keep network progress independent of the console renderer's frame wait.
    // Input and applet events remain serviced at least every idle poll interval.
    u64 now = armGetSystemTick();
    if (last_ui_tick && armTicksToNs(now - last_ui_tick) < 100000000ull) return running;
    last_ui_tick = now;
    if (!appletMainLoop()) running = false;
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) running = false;
    consoleUpdate(NULL);
    return running;
}

static bool receive_exact(int fd, void *data, size_t size, u64 timeout_ms) {
    u8 *position = data;
    u64 last_progress = armGetSystemTick();
    while (size && update_ui()) {
        if (armTicksToNs(armGetSystemTick() - last_progress) / 1000000 > timeout_ms) return false;
        struct pollfd event = {.fd = fd, .events = POLLIN};
        int ready = poll(&event, 1, 100);
        if (ready < 0) { if (errno == EINTR) continue; return false; }
        if (!ready) continue;
        if (event.revents & (POLLERR | POLLNVAL)) return false;
        ssize_t count = recv(fd, position, size, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0) return false;
        position += count;
        size -= (size_t)count;
        last_progress = armGetSystemTick();
    }
    return size == 0;
}

static bool send_exact(int fd, const void *data, size_t size) {
    const u8 *position = data;
    u64 last_progress = armGetSystemTick();
    while (size && update_ui()) {
        if (armTicksToNs(armGetSystemTick() - last_progress) / 1000000 > 15000) return false;
        struct pollfd event = {.fd = fd, .events = POLLOUT};
        int ready = poll(&event, 1, 100);
        if (ready < 0) { if (errno == EINTR) continue; return false; }
        if (!ready) continue;
        if (event.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
        ssize_t count = send(fd, position, size, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0) return false;
        position += count;
        size -= (size_t)count;
        last_progress = armGetSystemTick();
    }
    return size == 0;
}

static bool answer(int fd, const u8 *request, Result result, size_t size) {
    if (size > HCB_MAX_PACKET) return false;
    u8 *header = response_frame.header;
    memset(header, 0, HCB_HEADER_SIZE);
    hcb_put32(header, HCB_MAGIC);
    header[4] = HCB_VERSION;
    header[6] = request[6]; header[7] = request[7];
    hcb_put32(header + 8, hcb_u32(request + 8));
    hcb_put32(header + 12, (u32)size);
    hcb_put32(header + 16, result);
    return send_exact(fd, &response_frame, HCB_HEADER_SIZE + size);
}

static Resource *lookup(u32 id) {
    for (size_t i = 0; i < RESOURCE_LIMIT; i++) if (resources[i].kind && resources[i].id == id) return &resources[i];
    return NULL;
}

static unsigned available_slots(void) {
    unsigned count = 0;
    for (size_t i = 0; i < RESOURCE_LIMIT; i++) if (!resources[i].kind) count++;
    return count;
}

static Resource *new_resource(u32 kind) {
    if (next_id == UINT32_MAX) return NULL;
    for (size_t i = 0; i < RESOURCE_LIMIT; i++) {
        if (resources[i].kind) continue;
        memset(&resources[i], 0, sizeof(resources[i]));
        resources[i].id = ++next_id;
        resources[i].kind = kind;
        return &resources[i];
    }
    return NULL;
}

static Result release_resource(Resource *r) {
    if (r->kind == Resource_Service) serviceClose(&r->service);
    else if (r->kind == Resource_Handle) {
        Result rc = svcCloseHandle(r->handle);
        if (R_FAILED(rc)) return rc;
    } else if (r->kind == Resource_Buffer) { free(r->buffer); allocated -= r->size; }
    else if (r->kind == Resource_Jit) {
        Result rc = jitClose(&r->jit);
        if (R_FAILED(rc)) return rc;
        allocated -= r->size;
    }
    memset(r, 0, sizeof(*r));
    return 0;
}

static bool has_children(const Resource *r) {
    for (size_t i = 0; i < RESOURCE_LIMIT; i++) if (resources[i].kind && resources[i].parent == r->id) return true;
    return false;
}

static bool release_all(void) {
    /* Newer resources may be domain children. Retire them before their parents. */
    for (unsigned n = 0; n < RESOURCE_LIMIT; n++) {
        Resource *latest = NULL;
        for (size_t i = 0; i < RESOURCE_LIMIT; i++) {
            if (resources[i].kind && (!latest || resources[i].id > latest->id)) latest = &resources[i];
        }
        if (!latest) return true;
        Result rc = release_resource(latest);
        if (R_FAILED(rc)) {
            printf("Resource cleanup failed 0x%x. Exit the app before another client.\n", rc);
            return false;
        }
    }
    return true;
}

static void describe_resource(u8 *out, const Resource *r) {
    memset(out, 0, 32);
    hcb_put32(out, r->id); hcb_put32(out + 4, r->kind);
    if (r->kind == Resource_Service) {
        hcb_put32(out + 8, r->service.session);
        hcb_put32(out + 12, r->service.object_id);
    } else if (r->kind == Resource_Handle) hcb_put32(out + 8, r->handle);
    else {
        void *address = r->kind == Resource_Buffer ? r->buffer : jitGetRxAddr((Jit *)&r->jit);
        hcb_put64(out + 16, (u64)address); hcb_put64(out + 24, r->size);
    }
}

static Result open_hen(Service *out) {
    Service ro;
    Result rc = kaihenGetRoSession(&ro);
    if (R_SUCCEEDED(rc)) rc = kaihenOpenHomebrewSession(&ro, out);
    return rc;
}

static Result acquire_hen_resource(const char *name, Resource *out) {
    Service hen;
    Result rc = open_hen(&hen);
    if (R_FAILED(rc)) return rc;
    if (!strcmp(name, "hen")) { out->service = hen; return 0; }
    Handle handle = INVALID_HANDLE;
    if (!strcmp(name, "fs")) { rc = henGetFilesystemRoot(&hen, &handle); out->protected_identity = true; }
    else if (!strcmp(name, "settings")) rc = henGetSettingsSession(&hen, &handle);
    else if (!strcmp(name, "process")) { rc = henGetProcessHandle(&hen, &handle); out->kind = Resource_Handle; }
    else if (!strcmp(name, "sm")) rc = henGetServiceManager(&hen, &handle);
    else if (!strcmp(name, "loader")) { rc = henGetLoaderRoot(&hen, &handle); out->protected_identity = true; }
    else rc = HCB_BAD_REQUEST;
    serviceClose(&hen);
    if (R_SUCCEEDED(rc)) {
        if (out->kind == Resource_Handle) out->handle = handle;
        else serviceCreate(&out->service, handle);
    }
    return rc;
}

static Result memory_range(u64 address, u64 size, u32 permission) {
    if (!size || address > UINT64_MAX - size) return HCB_BAD_RANGE;
    u64 cursor = address, remaining = size;
    while (remaining) {
        MemoryInfo info; u32 page;
        Result rc = svcQueryMemory(&info, &page, cursor);
        if (R_FAILED(rc)) return rc;
        if ((info.perm & permission) != permission || cursor < info.addr || cursor - info.addr >= info.size) return HCB_BAD_RANGE;
        u64 span = info.size - (cursor - info.addr);
        if (span >= remaining) return 0;
        remaining -= span; cursor += span;
    }
    return 0;
}

static Result resource_handle(u32 id, Handle *out) {
    if (!id) { *out = CUR_PROCESS_HANDLE; return 0; }
    if (id == UINT32_MAX) { *out = CUR_THREAD_HANDLE; return 0; }
    Resource *r = lookup(id);
    if (!r) return HCB_BAD_RESOURCE;
    if (r->kind == Resource_Handle) *out = r->handle;
    else if (r->kind == Resource_Service) *out = r->service.session;
    else return HCB_BAD_RESOURCE;
    return 0;
}

/* Check the received handle counts and inline bounds before libounce parses it. */
static Result validate_ipc_reply(const HcbIpc *p, bool domain) {
    u8 *tls = armGetTls();
    HipcHeader header; memcpy(&header, tls, sizeof(header));
    size_t at = sizeof(header);
    u32 copies = 0, moves = 0;
    if (header.has_special_header) {
        HipcSpecialHeader special; memcpy(&special, tls + at, sizeof(special)); at += sizeof(special);
        if (special.send_pid) at += 8;
        copies = special.num_copy_handles; moves = special.num_move_handles;
    }
    size_t handles_at = at;
    at += 4 * (copies + moves) + 8 * header.num_send_statics;
    size_t raw_end = at + 4 * header.num_data_words;
    size_t start = (at + 15) & ~(size_t)15;
    if (raw_end > 0x100 || start + (domain ? 16 : 0) + 16 > raw_end ||
        header.num_send_buffers || header.num_recv_buffers || header.num_exch_buffers) return HCB_BAD_REPLY;
    CmifOutHeader cmif; memcpy(&cmif, tls + start + (domain ? 16 : 0), sizeof(cmif));
    u32 expected_copies = 0, expected_moves = domain ? 0 : p->output_object_count;
    for (u32 i = 0; i < p->output_handle_count; i++) {
        if (p->output_handle_attrs[i] == 1) expected_copies++; else expected_moves++;
    }
    bool valid = cmif.magic == CMIF_OUT_HEADER_MAGIC;
    if (valid && R_SUCCEEDED(cmif.result)) {
        valid = copies == expected_copies && moves == expected_moves;
        if (domain) {
            u32 objects = hcb_u32(tls + start);
            valid = valid && objects == p->output_object_count;
        }
        size_t required = start + (domain ? 16 : 0) + 16 + p->output_size + (domain ? 4 * p->output_object_count : 0);
        valid = valid && required <= raw_end;
    } else valid = valid && copies == 0 && moves == 0;
    if (!valid) {
        /* These are newly received handles, never the caller's existing handles. */
        for (u32 i = 0; i < copies + moves; i++) svcCloseHandle(hcb_u32(tls + handles_at + 4 * i));
        return HCB_BAD_REPLY;
    }
    return 0;
}

static Result ipc_request(size_t size, size_t *reply_size) {
    HcbIpc p;
    if (!hcb_parse_ipc(request_data, size, &p)) return HCB_BAD_REQUEST;
    Resource *target = lookup(p.resource);
    if (!target || target->kind != Resource_Service) return HCB_BAD_RESOURCE;
    bool domain = target->service.object_id != 0;
    if (!hcb_ipc_shape_valid(&p, domain)) return HCB_BAD_REQUEST;
    if (!hcb_ipc_pointer_budget_valid(&p, target->service.pointer_buffer_size)) return HCB_LIMIT;
    if (hcb_ipc_tls_size(&p, domain) > 0x100) return HCB_LIMIT;
    /* SetCurrentProcess on a shared FS/loader root changes every clone's identity. */
    if (target->protected_identity && p.command == 1) return HCB_IDENTITY;
    if (available_slots() < p.output_handle_count + p.output_object_count ||
        UINT32_MAX - next_id < p.output_handle_count + p.output_object_count) return HCB_LIMIT;
    SfDispatchParams dispatch = {0};
    Service output_objects[HCB_MAX_ITEMS] = {0};
    Handle output_handles[8] = {0};
    void *buffers[HCB_MAX_ITEMS] = {0};
    Result rc = 0;
    for (u32 i = 0; i < p.input_handle_count; i++) {
        Resource *r = lookup(p.input_handles[i]);
        if (!r || (r->kind != Resource_Handle && r->kind != Resource_Service)) { rc = HCB_BAD_RESOURCE; goto finish; }
        dispatch.in_handles[i] = r->kind == Resource_Handle ? r->handle : r->service.session;
    }
    for (u32 i = 0; i < p.input_object_count; i++) {
        Resource *r = lookup(p.input_objects[i]);
        if (!r || r->kind != Resource_Service || !r->service.object_id || r->service.session != target->service.session) {
            rc = HCB_BAD_RESOURCE; goto finish;
        }
        dispatch.in_objects[i] = &r->service;
    }
    size_t response_budget = 20 + 32 * (p.output_object_count + p.output_handle_count) + 4 * p.buffer_count + p.output_size;
    for (u32 i = 0; i < p.buffer_count; i++) {
        response_budget += p.buffer_sizes[i];
        if (response_budget > HCB_MAX_PACKET) { rc = HCB_LIMIT; goto finish; }
        size_t capacity = (p.buffer_sizes[i] + 4095) & ~(size_t)4095;
        buffers[i] = memalign(4096, capacity);
        if (!buffers[i]) { rc = HCB_LIMIT; goto finish; }
        memset(buffers[i], 0, capacity);
        if (p.buffer_attrs[i] & 1) memcpy(buffers[i], p.buffers[i], p.buffer_sizes[i]);
        memcpy((u8 *)&dispatch.buffer_attrs + 4 * i, &p.buffer_attrs[i], 4);
        dispatch.buffers[i] = (SfBuffer){buffers[i], p.buffer_sizes[i]};
    }
    dispatch.in_num_handles = p.input_handle_count;
    dispatch.in_num_objects = p.input_object_count;
    dispatch.in_send_pid = (p.flags & 1) != 0;
    dispatch.context = p.context;
    dispatch.out_num_objects = p.output_object_count;
    dispatch.out_objects = output_objects;
    dispatch.out_handles = output_handles;
    for (u32 i = 0; i < p.output_handle_count; i++) memcpy((u8 *)&dispatch.out_handle_attrs + 4 * i, &p.output_handle_attrs[i], 4);

    void *input = serviceMakeRequest(&target->service, p.command, p.context,
        p.input_size, dispatch.in_send_pid, dispatch.buffer_attrs, dispatch.buffers,
        p.input_object_count, dispatch.in_objects, p.input_handle_count, dispatch.in_handles);
    if (p.input_size) memcpy(input, p.input, p.input_size);
    Result ipc_result = svcSendSyncRequest(target->service.session);
    u8 inline_output[HCB_MAX_INLINE] = {0};
    if (R_SUCCEEDED(ipc_result)) {
        rc = validate_ipc_reply(&p, domain);
        if (R_FAILED(rc)) goto finish;
        void *output = NULL;
        ipc_result = serviceParseResponse(&target->service, p.output_size, &output,
            p.output_object_count, output_objects, dispatch.out_handle_attrs, output_handles);
        if (R_SUCCEEDED(ipc_result) && p.output_size) memcpy(inline_output, output, p.output_size);
    }
    u32 object_count = R_SUCCEEDED(ipc_result) ? p.output_object_count : 0;
    u32 handle_count = R_SUCCEEDED(ipc_result) ? p.output_handle_count : 0;
    u32 inline_size = R_SUCCEEDED(ipc_result) ? p.output_size : 0;
    hcb_put32(response_data, ipc_result); hcb_put32(response_data + 4, inline_size);
    hcb_put32(response_data + 8, object_count); hcb_put32(response_data + 12, handle_count);
    hcb_put32(response_data + 16, p.buffer_count);
    size_t at = 20;
    for (u32 i = 0; i < object_count; i++) {
        Resource *r = new_resource(Resource_Service);
        r->service = output_objects[i];
        if (domain) r->parent = target->id;
        describe_resource(response_data + at, r); at += 32;
    }
    for (u32 i = 0; i < handle_count; i++) {
        Resource *r = new_resource(Resource_Handle);
        r->handle = output_handles[i];
        describe_resource(response_data + at, r); at += 32;
    }
    for (u32 i = 0; i < p.buffer_count; i++) { hcb_put32(response_data + at, p.buffer_sizes[i]); at += 4; }
    memcpy(response_data + at, inline_output, inline_size); at += inline_size;
    for (u32 i = 0; i < p.buffer_count; i++) {
        memcpy(response_data + at, buffers[i], p.buffer_sizes[i]); at += p.buffer_sizes[i];
    }
    *reply_size = at;
finish:
    for (u32 i = 0; i < HCB_MAX_ITEMS; i++) free(buffers[i]);
    return rc;
}

static bool get_name(char *out, size_t capacity, size_t size) {
    if (!size || size >= capacity || memchr(request_data, 0, size)) return false;
    memcpy(out, request_data, size); out[size] = 0;
    for (size_t i = 0; i < size; i++) if ((u8)out[i] < 33 || (u8)out[i] > 126) return false;
    return true;
}

static Result process_request(u16 operation, size_t size, size_t *reply_size) {
    *reply_size = 0;
    if (operation == Hcb_Ipc) return ipc_request(size, reply_size);
    if (operation == Hcb_Status && !size) {
        u64 pid = 0, program = 0;
        Result pid_rc = svcGetProcessId(&pid, CUR_PROCESS_HANDLE);
        Result program_rc = svcGetInfo(&program, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0);
        int count = snprintf((char *)response_data, sizeof(response_data),
            "{\"version\":\"0.2.1\",\"henResult\":%u,\"henProtocol\":%u,"
            "\"henPid\":\"0x%lx\",\"henInitializationResult\":%u,\"processId\":\"0x%lx\","
            "\"processResult\":%u,\"programId\":\"0x%016lx\",\"programResult\":%u,"
            "\"runtimeHosVersion\":%u,\"resources\":%u,\"allocatedBytes\":%zu,\"requestsCompleted\":%lu,"
            "\"memoryScope\":\"bridge process\",\"authentication\":\"none\",\"maxPacket\":%u,\"port\":%u}",
            hen_result, hen_status.protocol_version, hen_status.process_id,
            hen_status.initialization_result, pid, pid_rc, program, program_rc,
            hosversionGet(), RESOURCE_LIMIT - available_slots(), allocated, requests_completed,
            HCB_MAX_PACKET, BRIDGE_PORT);
        if (count < 0 || (size_t)count >= sizeof(response_data)) return HCB_LIMIT;
        *reply_size = (size_t)count; return 0;
    }
    if (operation == Hcb_SyscallHints && !size) {
        for (unsigned i = 0; i < 256; i++) response_data[i] = envIsSyscallHinted(i) ? 1 : 0;
        *reply_size = 256; return 0;
    }
    if (operation == Hcb_Symbols && !size) {
        int count = snprintf((char *)response_data, sizeof(response_data),
            "{\"svcGetInfo\":\"0x%lx\",\"svcQueryMemory\":\"0x%lx\","
            "\"svcGetProcessId\":\"0x%lx\",\"svcGetSystemTick\":\"0x%lx\","
            "\"malloc\":\"0x%lx\",\"free\":\"0x%lx\",\"memcpy\":\"0x%lx\"}",
            (u64)(uintptr_t)svcGetInfo, (u64)(uintptr_t)svcQueryMemory,
            (u64)(uintptr_t)svcGetProcessId, (u64)(uintptr_t)svcGetSystemTick,
            (u64)(uintptr_t)malloc, (u64)(uintptr_t)free, (u64)(uintptr_t)memcpy);
        if (count < 0 || (size_t)count >= sizeof(response_data)) return HCB_LIMIT;
        *reply_size = (size_t)count; return 0;
    }
    if (operation == Hcb_Resources && !size) {
        size_t at = 0;
        for (unsigned i = 0; i < RESOURCE_LIMIT; i++) {
            if (!resources[i].kind) continue;
            describe_resource(response_data + at, &resources[i]); at += 32;
        }
        *reply_size = at; return 0;
    }
    if (operation == Hcb_GetInfo && size == 16) {
        Handle handle;
        Result rc = resource_handle(hcb_u32(request_data + 4), &handle);
        u64 value = 0;
        if (R_SUCCEEDED(rc)) rc = svcGetInfo(&value, hcb_u32(request_data), handle, hcb_u64(request_data + 8));
        if (R_SUCCEEDED(rc)) { hcb_put64(response_data, value); *reply_size = 8; }
        return rc;
    }
    if (operation == Hcb_QueryMemory && size == 8) {
        MemoryInfo info; u32 page;
        Result rc = svcQueryMemory(&info, &page, hcb_u64(request_data));
        if (R_SUCCEEDED(rc)) {
            memcpy(response_data, &info, sizeof(info)); hcb_put32(response_data + sizeof(info), page);
            *reply_size = sizeof(info) + 4;
        }
        return rc;
    }
    if (operation == Hcb_ReadMemory && size == 12) {
        u64 address = hcb_u64(request_data); u32 length = hcb_u32(request_data + 8);
        if (length > HCB_MAX_BUFFER) return HCB_LIMIT;
        Result rc = memory_range(address, length, Perm_R);
        if (R_SUCCEEDED(rc)) { memcpy(response_data, (void *)(uintptr_t)address, length); *reply_size = length; }
        return rc;
    }
    if (operation == Hcb_Allocate && size == 4) {
        size_t length = hcb_u32(request_data);
        if (!length || length > ALLOCATION_BUDGET - allocated) return HCB_LIMIT;
        Resource *r = new_resource(Resource_Buffer);
        if (!r) return HCB_LIMIT;
        r->buffer = calloc(1, length);
        if (!r->buffer) { memset(r, 0, sizeof(*r)); return HCB_LIMIT; }
        r->size = length; allocated += length;
        describe_resource(response_data, r); *reply_size = 32; return 0;
    }
    if (operation == Hcb_WriteAllocation && size >= 8) {
        Resource *r = lookup(hcb_u32(request_data));
        size_t offset = hcb_u32(request_data + 4), length = size - 8;
        if (!r || r->kind != Resource_Buffer) return HCB_BAD_RESOURCE;
        if (offset > r->size || length > r->size - offset) return HCB_BAD_RANGE;
        memcpy((u8 *)r->buffer + offset, request_data + 8, length);
        hcb_put32(response_data, (u32)length); *reply_size = 4; return 0;
    }
    if (operation == Hcb_OpenService || operation == Hcb_HenResource) {
        char name[16];
        if (!get_name(name, sizeof(name), size) || (operation == Hcb_OpenService && size > 8)) return HCB_BAD_REQUEST;
        Resource *r = new_resource(Resource_Service);
        if (!r) return HCB_LIMIT;
        Result rc;
        if (operation == Hcb_HenResource) rc = acquire_hen_resource(name, r);
        else if (!strcmp(name, "fsp-srv")) rc = acquire_hen_resource("fs", r);
        else if (!strcmp(name, "fsp-ldr")) rc = acquire_hen_resource("loader", r);
        else {
            rc = smGetService(&r->service, name);
            if (R_SUCCEEDED(rc) && serviceIsOverride(&r->service)) {
                Service cloned;
                rc = serviceClone(&r->service, &cloned);
                if (R_SUCCEEDED(rc)) r->service = cloned;
            }
        }
        if (R_FAILED(rc)) { memset(r, 0, sizeof(*r)); return rc; }
        describe_resource(response_data, r); *reply_size = 32; return 0;
    }
    if ((operation == Hcb_CloneService || operation == Hcb_Domain || operation == Hcb_HandleToService || operation == Hcb_Close) && size == 4) {
        Resource *r = lookup(hcb_u32(request_data));
        if (!r) return HCB_BAD_RESOURCE;
        if (operation == Hcb_Close) { if (has_children(r)) return HCB_BUSY; return release_resource(r); }
        if (operation == Hcb_HandleToService) {
            if (r->kind != Resource_Handle) return HCB_BAD_RESOURCE;
            serviceCreate(&r->service, r->handle); r->kind = Resource_Service;
            describe_resource(response_data, r); *reply_size = 32; return 0;
        }
        if (r->kind != Resource_Service) return HCB_BAD_RESOURCE;
        if (operation == Hcb_Domain) {
            if (has_children(r)) return HCB_BUSY;
            Result rc = serviceConvertToDomain(&r->service);
            if (R_SUCCEEDED(rc)) { describe_resource(response_data, r); *reply_size = 32; }
            return rc;
        }
        Resource *copy = new_resource(Resource_Service);
        if (!copy) return HCB_LIMIT;
        Result rc = serviceClone(&r->service, &copy->service);
        if (R_FAILED(rc)) { memset(copy, 0, sizeof(*copy)); return rc; }
        copy->protected_identity = r->protected_identity;
        describe_resource(response_data, copy); *reply_size = 32; return 0;
    }
    if (operation == Hcb_Jit) {
        if (!size || size > HCB_MAX_BUFFER || size % 4) return HCB_BAD_REQUEST;
        size_t capacity = (size + 4095) & ~(size_t)4095;
        if (capacity > ALLOCATION_BUDGET - allocated) return HCB_LIMIT;
        Resource *r = new_resource(Resource_Jit);
        if (!r) return HCB_LIMIT;
        Result rc = jitCreate(&r->jit, capacity);
        if (R_FAILED(rc)) { memset(r, 0, sizeof(*r)); return rc; }
        r->size = capacity; allocated += capacity;
        memset(jitGetRwAddr(&r->jit), 0, capacity);
        memcpy(jitGetRwAddr(&r->jit), request_data, size);
        rc = jitTransitionToExecutable(&r->jit);
        if (R_FAILED(rc)) { Result close_rc = release_resource(r); if (R_FAILED(close_rc)) running = false; return rc; }
        describe_resource(response_data, r); *reply_size = 32; return 0;
    }
    if (operation == Hcb_Call && size == 72) {
        u64 address = hcb_u64(request_data);
        if (address % 4) return HCB_BAD_RANGE;
        Result rc = memory_range(address, 4, Perm_X);
        if (R_FAILED(rc)) return rc;
        typedef u64 (*NativeFunction)(u64, u64, u64, u64, u64, u64, u64, u64);
        NativeFunction function = (NativeFunction)(uintptr_t)address;
        u64 result = function(hcb_u64(request_data + 8), hcb_u64(request_data + 16),
            hcb_u64(request_data + 24), hcb_u64(request_data + 32), hcb_u64(request_data + 40),
            hcb_u64(request_data + 48), hcb_u64(request_data + 56), hcb_u64(request_data + 64));
        hcb_put64(response_data, result); *reply_size = 8; return 0;
    }
    return HCB_BAD_REQUEST;
}

static void serve_client(int fd) {
    printf("LAN client connected.\n");
    while (running) {
        u8 header[HCB_HEADER_SIZE];
        if (!receive_exact(fd, header, sizeof(header), 300000)) break;
        if (!hcb_valid_header(header)) { answer(fd, header, HCB_BAD_REQUEST, 0); break; }
        u16 operation = header[6] | (u16)header[7] << 8;
        size_t size = hcb_u32(header + 12);
        if (size && !receive_exact(fd, request_data, size, 15000)) break;
        size_t reply_size = 0;
        Result rc = process_request(operation, size, &reply_size);
        requests_completed++;
        if (!answer(fd, header, rc, reply_size)) break;
    }
    if (!release_all()) running = false;
    printf("Client disconnected. Completed %lu requests.\n", requests_completed);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    printf("HEN command bridge 0.2.1\n\n");
    Service hen;
    hen_result = open_hen(&hen);
    if (R_SUCCEEDED(hen_result)) { hen_result = henGetStatus(&hen, &hen_status); serviceClose(&hen); }
    printf("HEN protocol %u, status 0x%x\n", hen_status.protocol_version, hen_result);
    Result rc = socketInitializeDefault();
    if (R_FAILED(rc)) { printf("Network initialization failed 0x%x\n", rc); goto finish; }
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    if (listener >= 0) setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(BRIDGE_PORT), .sin_addr = {.s_addr = INADDR_ANY}};
    if (listener < 0 || bind(listener, (void *)&address, sizeof(address)) < 0 || listen(listener, 1) < 0) {
        printf("Listener failed, errno %d\n", errno);
        if (listener >= 0) close(listener);
        socketExit(); goto finish;
    }
    u32 ip = gethostid();
    printf("Address %u.%u.%u.%u  TCP %u\n", ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, (ip >> 24) & 255, BRIDGE_PORT);
    printf("\nDirect LAN commands enabled.\nIPC and native calls can change persistent system state.\nPress + while idle to exit.\n\n");
    while (update_ui()) {
        struct pollfd event = {.fd = listener, .events = POLLIN};
        int ready = poll(&event, 1, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (event.revents & (POLLERR | POLLNVAL))) break;
        if (!ready) continue;
        int client = accept(listener, NULL, NULL);
        if (client < 0) continue;
        if (setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
            printf("Could not enable low-latency TCP, errno %d\n", errno);
        }
        int flags = fcntl(client, F_GETFL, 0);
        if (flags < 0 || fcntl(client, F_SETFL, flags | O_NONBLOCK) < 0) { close(client); continue; }
        serve_client(client);
        close(client);
    }
    close(listener);
    socketExit();
    consoleExit(NULL);
    return 0;
finish:
    printf("Press + to return.\n");
    while (update_ui()) svcSleepThread(100000000ull);
    consoleExit(NULL);
    return 1;
}
