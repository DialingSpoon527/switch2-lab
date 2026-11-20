#ifndef SEEDSOW_CLEANER_CORE_H
#define SEEDSOW_CLEANER_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define CLEAN_MAX_TARGETS 16
#define CLEAN_NAME_SIZE 22
#define CLEAN_PATH_SIZE 769
#define CLEAN_MAX_USERS 8
#define CLEAN_BAD_PLAN UINT32_C(0xf1020001)
#define CLEAN_BOUNDS UINT32_C(0xf1020002)
#define CLEAN_CHANGED UINT32_C(0xf1020003)
#define CLEAN_VERIFY UINT32_C(0xf1020004)
#define CLEAN_IO UINT32_C(0xf1020005)
typedef struct {
    uint64_t application, save_id, uid[2];
    uint8_t space, type, rank;
    uint16_t index;
    size_t count;
    char names[CLEAN_MAX_TARGETS][CLEAN_NAME_SIZE];
} CleanPlan;
typedef struct {
    void *context;
    uint32_t (*backup)(void *);
    uint32_t (*preflight)(void *);
    uint32_t (*remove_tree)(void *, const char *);
    uint32_t (*commit)(void *);
    uint32_t (*verify)(void *);
} CleanOps;
typedef struct {
    uint32_t result;
    bool backup_verified, write_started, committed, verified;
    size_t deleted;
    const char *stage;
    uint32_t audit_error;
} CleanReport;
typedef struct { char name[CLEAN_PATH_SIZE]; uint8_t type; int64_t size; } CleanNode;
typedef uint32_t (*CleanReadDirectory)(void *, const char *, CleanNode *, size_t, size_t *);
bool clean_application(uint64_t app);
bool clean_uid_is_active(const uint64_t uid[2], const uint64_t active[][2], size_t count);
bool clean_profile_check(const uint64_t save_uid[2], const uint64_t active[][2], size_t count, const uint64_t profile_record_uid[2], bool *record_differs);
bool clean_name(const char *name, char timestamp[14]);
bool clean_component(const char *name);
bool clean_join(char *out, size_t capacity, const char *parent, const char *name);
bool clean_plan_valid(const CleanPlan *plan);
bool clean_recognized(const char *name, CleanReadDirectory read_directory, void *context);
CleanReport clean_execute(const CleanPlan *plan, const CleanOps *ops);
#endif
