// This compiles the actual controller read/save functions extracted unchanged
// by test_controller_io.py. FS is a deterministic in-memory failure seam.
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "command_parser.h"
typedef uint32_t Result;
typedef uint64_t u64;
typedef int64_t s64;
typedef struct { int unused; } FsFileSystem;
typedef struct { int index; } FsFile;
#define R_VALUE(rc) ((rc) & 0x3fffff)
#define R_SUCCEEDED(rc) ((rc) == 0)
#define R_FAILED(rc) ((rc) != 0)
#define MAKERESULT(module, description) ((Result)(((description) << 9) | (module)))
enum { Module_Libnx = 345, LibnxError_BadInput = 1, FsOpenMode_Read = 1,
       FsOpenMode_Write = 2, FsReadOption_None = 0, FsWriteOption_Flush = 1 };
static const Result Failed = 0x123456;
typedef struct { bool exists; const char* path; char data[18000]; size_t length; } Entry;
static Entry files[4];
static const char* failure_operation;
static const char* failure_path;
static unsigned commits;

static bool fail(const char* operation, const char* path) {
    if (failure_operation && !strcmp(operation, failure_operation) &&
        (!failure_path || !strcmp(path, failure_path))) {
        failure_operation = NULL; // One failure; a recovery attempt can succeed.
        return true;
    }
    return false;
}
static int find(const char* path) {
    for (int i = 0; i < 4; ++i) if (!strcmp(files[i].path, path)) return i;
    assert(!"unexpected path"); return -1;
}
static Result fsFsOpenFile(FsFileSystem* sd, const char* path, int mode, FsFile* output) {
    (void)sd; (void)mode;
    if (fail("open", path)) return Failed;
    int index = find(path); if (!files[index].exists) return 0x202;
    output->index = index; return 0;
}
static Result fsFileGetSize(FsFile* file, s64* size) { *size = (s64)files[file->index].length; return 0; }
static Result fsFileRead(FsFile* file, u64 offset, void* output, u64 size, int flags, u64* actual) {
    (void)flags; assert(offset + size <= files[file->index].length);
    memcpy(output, files[file->index].data + offset, (size_t)size); *actual = size; return 0;
}
static void fsFileClose(FsFile* file) { (void)file; }
static Result fsFsDeleteFile(FsFileSystem* sd, const char* path) {
    (void)sd; if (fail("delete", path)) return Failed;
    int index = find(path); if (!files[index].exists) return 0x202;
    files[index].exists = false; return 0;
}
static Result fsFsCommit(FsFileSystem* sd) { (void)sd; ++commits; return fail("commit", "") ? Failed : 0; }
static Result fsFsCreateFile(FsFileSystem* sd, const char* path, u64 size, int flags) {
    (void)sd; (void)flags; if (fail("create", path)) return Failed;
    int index = find(path); assert(!files[index].exists && size < sizeof(files[index].data));
    files[index].exists = true; files[index].length = (size_t)size; memset(files[index].data, 0, sizeof(files[index].data)); return 0;
}
static Result fsFileWrite(FsFile* file, u64 offset, const void* input, u64 size, int flags) {
    (void)flags; Entry* entry = &files[file->index]; if (fail("write", entry->path)) return Failed;
    assert(offset + size <= entry->length); memcpy(entry->data + offset, input, (size_t)size); return 0;
}
static Result fsFsRenameFile(FsFileSystem* sd, const char* from, const char* to) {
    (void)sd; if (fail("rename", from)) return Failed;
    int source = find(from), destination = find(to);
    if (!files[source].exists) return 0x202;
    assert(!files[destination].exists);
    const char* path = files[destination].path;
    files[destination] = files[source]; files[destination].path = path;
    files[source].exists = false; return 0;
}
#include "controller_io_under_test.h"

static void reset(void) {
    memset(files, 0, sizeof(files));
    files[0].path = Command; files[1].path = ResultPath;
    files[2].path = ResultTemporary; files[3].path = ResultPrevious;
    failure_operation = failure_path = NULL; commits = 0; report_truncated = false; report_truncation_noted = false;
    strcpy(report, "new complete report\n");
}
static void put(int index, const void* data, size_t length) {
    files[index].exists = true; files[index].length = length;
    memcpy(files[index].data, data, length);
}
static void put_string(int index, const char* text) { put(index, text, strlen(text)); }
static bool equals(int index, const char* text) {
    return files[index].exists && files[index].length == strlen(text) && !memcmp(files[index].data, text, strlen(text));
}
int main(void) {
    FsFileSystem sd = {0}; char action[16], name[64];
    reset(); assert(readCommand(&sd, action, name) == 0 && !strcmp(action, "diag") && !name[0]);
    reset(); failure_operation = "open"; assert(readCommand(&sd, action, name) == Failed);
    reset(); put_string(0, "stop p2ping\n"); assert(readCommand(&sd, action, name) == 0);
    assert(!strcmp(action, "stop") && !strcmp(name, "p2ping") && !files[0].exists && commits == 1);
    reset(); const char hidden[] = "stop p2ping\0 launch p2ping"; put(0, hidden, sizeof(hidden) - 1);
    assert(readCommand(&sd, action, name) != 0 && !files[0].exists);
    reset(); char oversized[101]; memset(oversized, 'x', sizeof(oversized)); put(0, oversized, sizeof(oversized));
    assert(readCommand(&sd, action, name) != 0 && !files[0].exists);
    reset(); put_string(0, "stop p2ping"); failure_operation = "delete"; failure_path = Command;
    assert(readCommand(&sd, action, name) == Failed && files[0].exists && commits == 0);
    reset(); put_string(0, "stop p2ping"); failure_operation = "commit";
    assert(readCommand(&sd, action, name) == Failed && !files[0].exists);
    reset(); put_string(1, "old complete report\n"); assert(saveResult(&sd) == 0);
    assert(equals(1, report) && equals(3, "old complete report\n") && !files[2].exists && commits == 2);
    reset(); put_string(1, "old complete report\n"); failure_operation = "write"; failure_path = ResultTemporary;
    assert(saveResult(&sd) == Failed && equals(1, "old complete report\n"));
    reset(); put_string(1, "old complete report\n"); failure_operation = "commit";
    assert(saveResult(&sd) == Failed && equals(1, "old complete report\n") && equals(2, report));
    reset(); put_string(1, "old complete report\n"); failure_operation = "rename"; failure_path = ResultPath;
    assert(saveResult(&sd) == Failed && equals(1, "old complete report\n") && equals(2, report));
    reset(); put_string(1, "old complete report\n"); failure_operation = "rename"; failure_path = ResultTemporary;
    assert(saveResult(&sd) == Failed && equals(1, "old complete report\n") && equals(2, report) && commits == 2);
    reset(); memset(report, 'x', sizeof(report) - 65); report[sizeof(report) - 65] = 0; report_truncated = true;
    assert(saveResult(&sd) == 0); const size_t truncated_length = strlen(report);
    for (unsigned i = 0; i < 12; ++i) assert(saveResult(&sd) == 0 && strlen(report) == truncated_length);
    assert(truncated_length < sizeof(report));
    puts("actual controller FS failure-path tests passed");
    return 0;
}
