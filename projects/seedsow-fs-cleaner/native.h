#ifndef SEEDSOW_CLEANER_NATIVE_H
#define SEEDSOW_CLEANER_NATIVE_H
#include <switch.h>
#include "core.h"
#define CLEAN_MAX_SAVES 32
typedef struct {
    CleanPlan plan;
    char nickname[33];
    Result scan_result;
    unsigned unrecognized;
    bool profile_data_mismatch;
} CleanSave;
typedef void (*CleanProgress)(const char *, size_t, size_t);
Result cleaner_initialize(u32 *protocol);
void cleaner_exit(void);
Result cleaner_scan(CleanSave saves[CLEAN_MAX_SAVES], size_t *count);
CleanReport cleaner_clean(const CleanSave *save, char backup_path[FS_MAX_PATH], CleanProgress progress);
#endif
