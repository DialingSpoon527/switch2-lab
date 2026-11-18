#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "protocol.h"
#include <switch/sf/service.h>

static void test_parser(void) {
    uint8_t packet[128] = {0};
    hcb_put32(packet, 1); hcb_put32(packet + 4, 42);
    hcb_put32(packet + 16, 4); hcb_put32(packet + 20, 16);
    hcb_put32(packet + 24, 1); hcb_put32(packet + 28, 1);
    hcb_put32(packet + 32, 1); hcb_put32(packet + 36, 1);
    hcb_put32(packet + 40, 1);
    hcb_put32(packet + 48, 26); hcb_put32(packet + 52, 16);
    hcb_put32(packet + 56, 2); hcb_put32(packet + 60, 1);
    hcb_put32(packet + 64, 3); hcb_put32(packet + 68, 0x12345678);
    HcbIpc parsed;
    assert(hcb_parse_ipc(packet, 88, &parsed));
    assert(parsed.input_handles[0] == 2 && parsed.input_objects[0] == 3);
    assert(hcb_u32(parsed.input) == 0x12345678 && parsed.buffer_sizes[0] == 16);
    for (size_t length = 0; length < 88; length++) assert(!hcb_parse_ipc(packet, length, &parsed));
    assert(!hcb_parse_ipc(packet, 89, &parsed));
    hcb_put32(packet + 24, UINT32_MAX); assert(!hcb_parse_ipc(packet, 88, &parsed));
    hcb_put32(packet + 24, 1);
    hcb_put32(packet + 52, UINT32_MAX); assert(!hcb_parse_ipc(packet, 88, &parsed));
    hcb_put32(packet + 52, 16);
    hcb_put32(packet + 48, 12); assert(!hcb_parse_ipc(packet, 88, &parsed));
    uint8_t header[24] = {0};
    hcb_put32(header, HCB_MAGIC); header[4] = HCB_VERSION;
    assert(hcb_valid_header(header));
    hcb_put32(header + 12, HCB_MAX_PACKET + 1); assert(!hcb_valid_header(header));
    hcb_put32(header + 12, 0); hcb_put32(header + 20, 1); assert(!hcb_valid_header(header));
    hcb_put32(header + 20, 0); header[4] = 1; assert(!hcb_valid_header(header));
}

static void test_tls_layout(void) {
    const uint32_t attrs[] = {5, 6, 7, 9, 10, 11, 26, 33, 34, 35};
    unsigned accepted = 0, refused = 0, invalid = 0;
    for (unsigned iteration = 0; iteration < 20000; iteration++) {
        HcbIpc p = {0};
        p.input_size = (iteration * 17) % 129;
        p.input_handle_count = (iteration / 3) % 5;
        p.input_object_count = (iteration / 7) % 5;
        p.buffer_count = iteration % 5;
        p.flags = iteration & 1;
        bool domain = (iteration & 2) != 0;
        if (!hcb_ipc_shape_valid(&p, domain)) { invalid++; continue; }
        CmifRequestFormat format = {0};
        format.object_id = domain ? 1 : 0;
        format.data_size = p.input_size;
        format.num_handles = p.input_handle_count;
        format.num_objects = p.input_object_count;
        format.send_pid = p.flags;
        for (unsigned i = 0; i < p.buffer_count; i++) {
            p.buffer_attrs[i] = attrs[(iteration / (i + 1)) % 10];
            _serviceRequestFormatProcessBuffer(&format, p.buffer_attrs[i]);
        }
        uint8_t scratch[4096] __attribute__((aligned(16)));
        memset(scratch, 0xa5, sizeof(scratch));
        CmifRequest request = cmifMakeRequest(scratch, format);
        size_t receive_count = format.num_out_auto_buffers + format.num_out_pointers + format.num_out_fixed_pointers;
        HipcHeader *header = (HipcHeader *)scratch;
        size_t actual = (uint8_t *)(request.hipc.data_words + header->num_data_words) - scratch + receive_count * sizeof(HipcRecvListEntry);
        if (hcb_ipc_tls_size(&p, domain) != actual) {
            printf("Layout mismatch at %u, predicted %zu, actual %zu, domain %u, buffers %u, handles %u, objects %u, pid %u\n",
                iteration, hcb_ipc_tls_size(&p, domain), actual, domain, p.buffer_count,
                p.input_handle_count, p.input_object_count, p.flags);
            fflush(stdout);
            abort();
        }
        if (actual <= 0x100) {
            accepted++;
            memset(request.data, 0x55, p.input_size);
            if (domain) for (unsigned i = 0; i < p.input_object_count; i++) cmifRequestObject(&request, i + 1);
            for (unsigned i = 0; i < p.input_handle_count; i++) cmifRequestHandle(&request, i + 1);
            for (unsigned i = 0; i < p.buffer_count; i++) {
                SfBuffer buffer = {scratch + 1024, 32};
                _serviceRequestProcessBuffer(&request, &buffer, p.buffer_attrs[i]);
            }
            for (unsigned i = 0x100; i < 1024; i++) assert(scratch[i] == 0xa5);
        } else refused++;
    }
    assert(accepted && refused);
    printf("PASS: %u accepted and %u oversized CMIF layouts checked against the devkit serializer, %u invalid object layouts rejected\n", accepted, refused, invalid);
}

static void test_ipc_constraints(void) {
    HcbIpc p = {0};
    p.input_object_count = 1;
    p.input_size = 1;
    assert(!hcb_ipc_shape_valid(&p, true));
    p.input_size = 4;
    assert(hcb_ipc_shape_valid(&p, true));
    assert(!hcb_ipc_shape_valid(&p, false));
    p.output_object_count = 1;
    p.output_size = 3;
    assert(!hcb_ipc_shape_valid(&p, true));
    p.output_size = 4;
    assert(hcb_ipc_shape_valid(&p, true));
    p.buffer_count = 1;
    p.buffer_attrs[0] = 9;
    p.buffer_sizes[0] = 17;
    assert(!hcb_ipc_pointer_budget_valid(&p, 16));
    p.buffer_sizes[0] = 8;
    p.buffer_attrs[0] = 11;
    assert(hcb_ipc_pointer_budget_valid(&p, 16));
    assert(!hcb_ipc_pointer_budget_valid(&p, 15));
    p.buffer_attrs[0] = 35;
    p.buffer_sizes[0] = 12;
    assert(hcb_ipc_pointer_budget_valid(&p, 16));
}

int main(void) {
    test_parser();
    test_ipc_constraints();
    test_tls_layout();
    puts("PASS: truncated/oversized IPC, attribute validation, and protocol-version headers");
    return 0;
}
