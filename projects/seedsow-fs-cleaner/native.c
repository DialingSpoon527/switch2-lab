#include "native.h"
#include <kaihen.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_ENTRIES 4096
#define MAX_COPY_BYTES (UINT64_C(512)*1024*1024)
#define CHUNK 65536
#define PATH_NOT_FOUND 0x202
_Static_assert(sizeof(FsSaveDataInfo)==96, "Save metadata layout");
_Static_assert(sizeof(FsSaveDataAttribute)==64, "Save attributes layout");
_Static_assert(sizeof(AccountProfileBase)==56, "Account profile layout");
_Static_assert(FS_MAX_PATH==CLEAN_PATH_SIZE, "Path policy matches SDK");
static Service fs_root;
static bool root_active;
static u8 transfer[CHUNK];

typedef struct {
    char path[FS_MAX_PATH];
    s64 size;
    u8 type, hash[32];
} Entry;
typedef struct {
    CleanPlan plan;
    FsFileSystem save, sd;
    bool save_active, sd_active;
    Entry *entries, *check;
    size_t count;
    u64 bytes;
    char backup[FS_MAX_PATH];
    FsFile journal;
    bool journal_active;
    s64 journal_offset;
    CleanProgress progress;
    Result audit_error;
} Context;

static Result read_active_users(Service *accounts, uint64_t active[CLEAN_MAX_USERS][2]) {
    memset(active,0,sizeof(uint64_t)*CLEAN_MAX_USERS*2);
    return serviceDispatch(accounts,2,
        .buffer_attrs={SfBufferAttr_Out|SfBufferAttr_HipcPointer},
        .buffers={{active,sizeof(uint64_t)*CLEAN_MAX_USERS*2}});
}
static Result check_active_user(const CleanPlan *plan) {
    Service accounts;
    Result rc=smGetService(&accounts,"acc:u0");
    if (R_FAILED(rc)) return rc;
    uint64_t active[CLEAN_MAX_USERS][2];
    rc=read_active_users(&accounts,active);
    serviceClose(&accounts);
    if (R_SUCCEEDED(rc) && !clean_uid_is_active(plan->uid,active,CLEAN_MAX_USERS)) rc=CLEAN_CHANGED;
    return rc;
}
static Result check_identity(const CleanPlan *plan) {
    Result rc=check_active_user(plan);
    if (R_FAILED(rc)) return rc;
    FsSaveDataInfoReader reader; u8 space=1;
    rc=serviceDispatchIn(&fs_root,61,space,.out_num_objects=1,.out_objects=&reader.s);
    if (R_FAILED(rc)) return rc;
    unsigned matches=0; bool complete=false;
    for (unsigned page=0;page<64;page++) {
        FsSaveDataInfo rows[128]; s64 n=0;
        rc=fsSaveDataInfoReaderRead(&reader,rows,128,&n);
        if (R_FAILED(rc)) break;
        if (n<0 || n>128) { rc=CLEAN_BOUNDS; break; }
        if (!n) { complete=true; break; }
        for (s64 i=0;i<n;i++) {
            const FsSaveDataInfo *s=&rows[i];
            if (s->application_id!=plan->application || memcmp(&s->uid,plan->uid,sizeof(s->uid)) ||
                s->save_data_space_id!=1 || s->save_data_type!=1 || s->save_data_rank || s->save_data_index) continue;
            matches++;
            if (s->save_data_id!=plan->save_id || s->system_save_data_id) rc=CLEAN_CHANGED;
        }
        if (R_FAILED(rc)) break;
    }
    fsSaveDataInfoReaderClose(&reader);
    if (R_SUCCEEDED(rc) && (!complete || matches!=1)) rc=CLEAN_CHANGED;
    return rc;
}

static Result open_save(const CleanPlan *plan, bool read_only, FsFileSystem *out) {
    struct { u8 space, pad[7]; FsSaveDataAttribute attr; } in={0};
    in.space=1; in.attr.application_id=plan->application;
    memcpy(&in.attr.uid, plan->uid, sizeof(in.attr.uid));
    in.attr.save_data_type=1;
    return serviceDispatchIn(&fs_root, read_only ? 53 : 51, in,
        .out_num_objects=1, .out_objects=&out->s);
}
Result cleaner_initialize(u32 *protocol) {
    Service ro, hen;
    Result rc=kaihenGetRoSession(&ro);
    if (R_FAILED(rc)) return rc;
    rc=kaihenOpenHomebrewSession(&ro,&hen);
    if (R_FAILED(rc)) return rc;
    HenStatus status={0};
    rc=henGetStatus(&hen,&status);
    if (R_SUCCEEDED(rc)) rc=status.initialization_result;
    if (R_SUCCEEDED(rc)) {
        Handle handle=INVALID_HANDLE;
        rc=henGetFilesystemRoot(&hen,&handle);
        if (R_SUCCEEDED(rc)) {
            serviceCreate(&fs_root,handle); root_active=true;
            rc=serviceConvertToDomain(&fs_root);
        }
    }
    *protocol=status.protocol_version;
    serviceClose(&hen);
    return rc;
}
void cleaner_exit(void) { if (root_active) { serviceClose(&fs_root); root_active=false; } }

static bool valid_entry(const FsDirectoryEntry *entry) {
    return memchr(entry->name,0,sizeof(entry->name)) && clean_component(entry->name) &&
        (entry->type==FsDirEntryType_Dir || entry->type==FsDirEntryType_File) && entry->file_size>=0;
}
static Result small_directory(FsFileSystem *fs, const char *path, FsDirectoryEntry *entries, size_t capacity, size_t *count) {
    FsDir dir;
    Result rc=fsFsOpenDirectory(fs,path,FsDirOpenMode_ReadDirs|FsDirOpenMode_ReadFiles,&dir);
    if (R_FAILED(rc)) return rc;
    *count=0;
    while (R_SUCCEEDED(rc)) {
        FsDirectoryEntry entry; s64 n=0;
        rc=fsDirRead(&dir,&n,1,&entry);
        if (R_FAILED(rc) || n==0) break;
        if (n!=1 || !valid_entry(&entry) || *count>=capacity) { rc=CLEAN_BOUNDS; break; }
        entries[(*count)++]=entry;
    }
    fsDirClose(&dir); return rc;
}
static uint32_t read_policy_directory(void *context, const char *path, CleanNode *nodes, size_t capacity, size_t *count) {
    if (capacity>4) return CLEAN_BOUNDS;
    FsDirectoryEntry entries[4];
    Result rc=small_directory(context,path,entries,capacity,count);
    if (R_FAILED(rc)) return rc;
    for (size_t i=0;i<*count;i++) {
        strcpy(nodes[i].name,entries[i].name);
        nodes[i].type=entries[i].type; nodes[i].size=entries[i].file_size;
    }
    return 0;
}
static Result find_targets(FsFileSystem *fs, CleanPlan *plan, unsigned *unrecognized) {
    FsDir dir; Result rc=fsFsOpenDirectory(fs,"/",3,&dir);
    if (R_FAILED(rc)) return rc;
    plan->count=0; *unrecognized=0;
    bool complete=false;
    for (unsigned i=0;i<MAX_ENTRIES;i++) {
        FsDirectoryEntry e; s64 n=0;
        rc=fsDirRead(&dir,&n,1,&e);
        if (R_FAILED(rc)) break;
        if (!n) { complete=true; break; }
        if (n!=1 || !valid_entry(&e)) { rc=CLEAN_BAD_PLAN; break; }
        if (e.type!=0 || strncmp(e.name,"NS2_GAP_",8)) continue;
        if (!clean_recognized(e.name,read_policy_directory,fs)) { (*unrecognized)++; continue; }
        if (plan->count==CLEAN_MAX_TARGETS) { rc=CLEAN_BOUNDS; break; }
        strcpy(plan->names[plan->count++],e.name);
    }
    if (R_SUCCEEDED(rc) && !complete) rc=CLEAN_BOUNDS;
    fsDirClose(&dir);
    return rc;
}
static Result profile_name(Service *accounts, AccountUid uid, const uint64_t active[][2], char nickname[33], bool *data_mismatch) {
    uint64_t requested[2]; memcpy(requested,&uid,sizeof(requested));
    if (!clean_uid_is_active(requested,active,CLEAN_MAX_USERS)) return CLEAN_CHANGED;
    Service profile;
    Result rc=serviceDispatchIn(accounts,5,uid,.out_num_objects=1,.out_objects=&profile);
    if (R_FAILED(rc)) return rc;
    AccountProfileBase base={0};
    rc=serviceDispatchOut(&profile,1,base);
    serviceClose(&profile);
    if (R_FAILED(rc)) return rc;
    // The name payload can contain a stale ID. Never use that ID to choose a
    // save. Selection uses the active lookup ID intersected with save ownership.
    uint64_t record_uid[2]; memcpy(record_uid,&base.uid,sizeof(record_uid));
    if (!clean_profile_check(requested,active,CLEAN_MAX_USERS,record_uid,data_mismatch)) return CLEAN_CHANGED;
    memcpy(nickname,base.nickname,32); nickname[32]=0;
    for (unsigned i=0;nickname[i];i++) if ((u8)nickname[i]<32 || (u8)nickname[i]==127) nickname[i]='?';
    return 0;
}
Result cleaner_scan(CleanSave saves[CLEAN_MAX_SAVES], size_t *count) {
    *count=0;
    Service accounts;
    Result rc=smGetService(&accounts,"acc:u0");
    if (R_FAILED(rc)) return rc;
    uint64_t active[CLEAN_MAX_USERS][2];
    rc=read_active_users(&accounts,active);
    if (R_FAILED(rc)) { serviceClose(&accounts); return rc; }
    FsSaveDataInfoReader reader;
    u8 space=1;
    rc=serviceDispatchIn(&fs_root,61,space,.out_num_objects=1,.out_objects=&reader.s);
    if (R_FAILED(rc)) { serviceClose(&accounts); return rc; }
    bool complete=false;
    for (unsigned page=0;page<64;page++) {
        FsSaveDataInfo rows[128]; s64 n=0;
        rc=fsSaveDataInfoReaderRead(&reader,rows,128,&n);
        if (R_FAILED(rc)) break;
        if (n<0 || n>128) { rc=CLEAN_BOUNDS; break; }
        if (!n) { complete=true; break; }
        for (s64 i=0;i<n;i++) {
            FsSaveDataInfo *s=&rows[i];
            if (!clean_application(s->application_id) || s->save_data_space_id!=1 || s->save_data_type!=1 ||
                s->save_data_rank || s->save_data_index || s->system_save_data_id || !accountUidIsValid(&s->uid)) continue;
            if (*count>=CLEAN_MAX_SAVES) { rc=CLEAN_BOUNDS; break; }
            CleanSave *out=&saves[(*count)++]; memset(out,0,sizeof(*out));
            out->plan.application=s->application_id; out->plan.save_id=s->save_data_id;
            memcpy(out->plan.uid,&s->uid,sizeof(s->uid)); out->plan.space=1; out->plan.type=1;
            out->scan_result=profile_name(&accounts,s->uid,active,out->nickname,&out->profile_data_mismatch);
            if (R_FAILED(out->scan_result)) { strcpy(out->nickname,"Unavailable profile"); continue; }
            FsFileSystem save;
            out->scan_result=open_save(&out->plan,true,&save);
            if (R_SUCCEEDED(out->scan_result)) {
                out->scan_result=find_targets(&save,&out->plan,&out->unrecognized);
                fsFsClose(&save);
            }
        }
        if (R_FAILED(rc)) break;
    }
    if (R_SUCCEEDED(rc) && !complete) rc=CLEAN_BOUNDS;
    fsSaveDataInfoReaderClose(&reader); serviceClose(&accounts);
    return rc;
}

static bool excluded(const CleanPlan *plan, const char *path) {
    if (path[0]!='/' || strchr(path+1,'/')) return false;
    for (size_t i=0;i<plan->count;i++) if (!strcmp(path+1,plan->names[i])) return true;
    return false;
}
static int entry_order(const void *a, const void *b) { return strcmp(((const Entry *)a)->path,((const Entry *)b)->path); }
static Result inventory(FsFileSystem *fs, const CleanPlan *plan, Entry *entries, size_t *count, u64 *bytes) {
    *count=1; *bytes=0;
    memset(&entries[0],0,sizeof(Entry)); strcpy(entries[0].path,"/");
    for (size_t at=0;at<*count;at++) {
        if (entries[at].type!=0) continue;
        FsDir dir; Result rc=fsFsOpenDirectory(fs,entries[at].path,3,&dir);
        if (R_FAILED(rc)) return rc;
        bool complete=false;
        for (unsigned k=0;k<MAX_ENTRIES;k++) {
            FsDirectoryEntry e; s64 n=0;
            rc=fsDirRead(&dir,&n,1,&e);
            if (R_FAILED(rc)) break;
            if (!n) { complete=true; break; }
            char name[FS_MAX_PATH];
            if (n!=1 || !valid_entry(&e) || !clean_join(name,sizeof(name),entries[at].path,e.name)) { rc=CLEAN_BOUNDS; break; }
            if (excluded(plan,name)) continue;
            if (*count>=MAX_ENTRIES) { rc=CLEAN_BOUNDS; break; }
            Entry *out=&entries[(*count)++]; memset(out,0,sizeof(*out));
            strcpy(out->path,name); out->size=e.type==1 ? e.file_size : 0; out->type=e.type;
            if (e.type==1) {
                if ((u64)e.file_size>MAX_COPY_BYTES-*bytes) { rc=CLEAN_BOUNDS; break; }
                *bytes+=(u64)e.file_size;
            }
        }
        if (R_SUCCEEDED(rc) && !complete) rc=CLEAN_BOUNDS;
        fsDirClose(&dir);
        if (R_FAILED(rc)) return rc;
    }
    qsort(entries,*count,sizeof(Entry),entry_order);
    for (size_t i=1;i<*count;i++) if (!strcmp(entries[i-1].path,entries[i].path)) return CLEAN_CHANGED;
    return 0;
}
static Result hash_file(FsFileSystem *fs, const char *path, s64 expected, u8 hash[32]) {
    FsFile file; Result rc=fsFsOpenFile(fs,path,FsOpenMode_Read,&file);
    if (R_FAILED(rc)) return rc;
    s64 size=0; rc=fsFileGetSize(&file,&size);
    if (R_SUCCEEDED(rc) && (size!=expected || size<0 || (u64)size>MAX_COPY_BYTES)) rc=CLEAN_CHANGED;
    Sha256Context digest; sha256ContextCreate(&digest);
    for (s64 at=0;R_SUCCEEDED(rc) && at<size;) {
        u64 chunk=(u64)(size-at)>CHUNK ? CHUNK : (u64)(size-at), got=0;
        rc=fsFileRead(&file,at,transfer,chunk,0,&got);
        if (R_SUCCEEDED(rc) && got!=chunk) rc=CLEAN_IO;
        if (R_SUCCEEDED(rc)) { sha256ContextUpdate(&digest,transfer,chunk); at+=(s64)chunk; }
    }
    fsFileClose(&file);
    if (R_SUCCEEDED(rc)) sha256ContextGetHash(&digest,hash);
    return rc;
}
static Result ensure_directory(FsFileSystem *fs, const char *path) {
    FsDirEntryType type; Result rc=fsFsGetEntryType(fs,path,&type);
    if (R_SUCCEEDED(rc)) return type==FsDirEntryType_Dir ? 0 : CLEAN_BAD_PLAN;
    if (rc!=PATH_NOT_FOUND) return rc;
    return fsFsCreateDirectory(fs,path);
}
static Result append_file(FsFile *file, s64 *offset, const char *text, bool flush) {
    size_t size=strlen(text);
    Result rc=fsFileWrite(file,*offset,text,size,flush ? FsWriteOption_Flush : 0);
    if (R_SUCCEEDED(rc)) *offset+=(s64)size;
    return rc;
}
static Result journal(Context *ctx, const char *text) { return append_file(&ctx->journal,&ctx->journal_offset,text,true); }
static Result create_log(FsFileSystem *sd, const char *path, FsFile *out) {
    Result rc=fsFsCreateFile(sd,path,0,0);
    if (R_SUCCEEDED(rc)) rc=fsFsOpenFile(sd,path,FsOpenMode_Write|FsOpenMode_Append,out);
    return rc;
}
static void hex_hash(char out[65], const u8 hash[32]) {
    static const char alphabet[]="0123456789abcdef";
    for (unsigned i=0;i<32;i++) { out[2*i]=alphabet[hash[i]>>4]; out[2*i+1]=alphabet[hash[i]&15]; }
    out[64]=0;
}
static bool sd_path(char out[FS_MAX_PATH], Context *ctx, const char *path) {
    int n=snprintf(out,FS_MAX_PATH,"%s/files%s",ctx->backup,path);
    return n>=0 && n<FS_MAX_PATH;
}
static Result backup(void *v) {
    Context *ctx=v;
    Result rc=check_identity(&ctx->plan);
    if (R_FAILED(rc)) return rc;
    rc=open_save(&ctx->plan,true,&ctx->save);
    if (R_FAILED(rc)) return rc;
    ctx->save_active=true;
    CleanPlan now=ctx->plan; unsigned unknown=0;
    rc=find_targets(&ctx->save,&now,&unknown);
    if (R_FAILED(rc) || now.count!=ctx->plan.count) return R_FAILED(rc) ? rc : CLEAN_CHANGED;
    for (size_t i=0;i<now.count;i++) {
        bool found=false;
        for (size_t j=0;j<ctx->plan.count;j++) if (!strcmp(now.names[i],ctx->plan.names[j])) found=true;
        if (!found) return CLEAN_CHANGED;
    }
    rc=inventory(&ctx->save,&ctx->plan,ctx->entries,&ctx->count,&ctx->bytes);
    if (R_FAILED(rc)) return rc;
    rc=serviceDispatch(&fs_root,18,.out_num_objects=1,.out_objects=&ctx->sd.s);
    if (R_FAILED(rc)) return rc;
    ctx->sd_active=true;
    s64 free_bytes=0; rc=fsFsGetFreeSpace(&ctx->sd,"/",&free_bytes);
    if (R_FAILED(rc)) return rc;
    if (free_bytes<0 || (u64)free_bytes<ctx->bytes+8*1024*1024) return CLEAN_BOUNDS;
    const char *parents[]={"/switch","/switch/seedsow-fs-cleaner","/switch/seedsow-fs-cleaner/backups"};
    for (size_t i=0;i<3;i++) if (R_FAILED(rc=ensure_directory(&ctx->sd,parents[i]))) return rc;
    snprintf(ctx->backup,sizeof(ctx->backup),"/switch/seedsow-fs-cleaner/backups/%016" PRIx64 "-%016" PRIx64,ctx->plan.save_id,armGetSystemTick());
    rc=fsFsCreateDirectory(&ctx->sd,ctx->backup);
    if (R_FAILED(rc)) return rc;
    char name[FS_MAX_PATH];
    if (!clean_join(name,sizeof(name),ctx->backup,"operation.log")) return CLEAN_BOUNDS;
    rc=create_log(&ctx->sd,name,&ctx->journal);
    if (R_FAILED(rc)) return rc;
    ctx->journal_active=true;
    if (R_FAILED(rc=journal(ctx,"Backup started. No save writes yet.\n"))) return rc;
    if (!clean_join(name,sizeof(name),ctx->backup,"files")) return CLEAN_BOUNDS;
    if (R_FAILED(rc=fsFsCreateDirectory(&ctx->sd,name))) return rc;
    char manifest_path[FS_MAX_PATH];
    if (!clean_join(manifest_path,sizeof(manifest_path),ctx->backup,"manifest.tsv")) return CLEAN_BOUNDS;
    FsFile manifest; rc=create_log(&ctx->sd,manifest_path,&manifest);
    if (R_FAILED(rc)) return rc;
    s64 manifest_offset=0; Sha256Context manifest_digest; sha256ContextCreate(&manifest_digest);
    char line[FS_MAX_PATH+160];
    snprintf(line,sizeof(line),"Seedsow FS cleaner 0.1.2\nApp=%016" PRIx64 " Save=%016" PRIx64 " UID=%016" PRIx64 "%016" PRIx64 "\nPartial save copy: recognized scratch trees excluded.\n",ctx->plan.application,ctx->plan.save_id,ctx->plan.uid[0],ctx->plan.uid[1]);
    rc=append_file(&manifest,&manifest_offset,line,false); sha256ContextUpdate(&manifest_digest,line,strlen(line));
    for (size_t i=1;R_SUCCEEDED(rc) && i<ctx->count;i++) {
        Entry *entry=&ctx->entries[i];
        if (ctx->progress) ctx->progress("Copying retained save files",i,ctx->count-1);
        if (!sd_path(name,ctx,entry->path)) { rc=CLEAN_BOUNDS; break; }
        if (entry->type==0) {
            rc=fsFsCreateDirectory(&ctx->sd,name);
            snprintf(line,sizeof(line),"D\t%s\n",entry->path);
        } else {
            FsFile input,output;
            rc=fsFsOpenFile(&ctx->save,entry->path,FsOpenMode_Read,&input);
            if (R_FAILED(rc)) break;
            s64 size=0; rc=fsFileGetSize(&input,&size);
            if (R_SUCCEEDED(rc) && size!=entry->size) rc=CLEAN_CHANGED;
            if (R_SUCCEEDED(rc)) rc=fsFsCreateFile(&ctx->sd,name,entry->size,0);
            bool output_active=false;
            if (R_SUCCEEDED(rc)) { rc=fsFsOpenFile(&ctx->sd,name,FsOpenMode_Write,&output); output_active=R_SUCCEEDED(rc); }
            Sha256Context digest; sha256ContextCreate(&digest);
            for (s64 at=0;R_SUCCEEDED(rc) && at<entry->size;) {
                u64 length=(u64)(entry->size-at)>CHUNK ? CHUNK : (u64)(entry->size-at),got=0;
                rc=fsFileRead(&input,at,transfer,length,0,&got);
                if (R_SUCCEEDED(rc) && got!=length) rc=CLEAN_IO;
                if (R_SUCCEEDED(rc)) rc=fsFileWrite(&output,at,transfer,length,0);
                if (R_SUCCEEDED(rc)) { sha256ContextUpdate(&digest,transfer,length); at+=(s64)length; }
            }
            if (R_SUCCEEDED(rc)) rc=fsFileFlush(&output);
            if (output_active) fsFileClose(&output);
            fsFileClose(&input);
            if (R_FAILED(rc)) break;
            sha256ContextGetHash(&digest,entry->hash);
            u8 check[32];
            rc=hash_file(&ctx->sd,name,entry->size,check);
            if (R_SUCCEEDED(rc) && memcmp(check,entry->hash,32)) rc=CLEAN_VERIFY;
            if (R_SUCCEEDED(rc)) rc=hash_file(&ctx->save,entry->path,entry->size,check);
            if (R_SUCCEEDED(rc) && memcmp(check,entry->hash,32)) rc=CLEAN_CHANGED;
            char hash[65]; hex_hash(hash,entry->hash);
            snprintf(line,sizeof(line),"F\t%" PRId64 "\t%s\t%s\n",entry->size,hash,entry->path);
        }
        if (R_SUCCEEDED(rc)) { rc=append_file(&manifest,&manifest_offset,line,false); sha256ContextUpdate(&manifest_digest,line,strlen(line)); }
    }
    if (R_SUCCEEDED(rc)) {
        strcpy(line,"Retained file copy complete and verified. Scratch contents were not backed up.\n");
        rc=append_file(&manifest,&manifest_offset,line,false); sha256ContextUpdate(&manifest_digest,line,strlen(line));
    }
    if (R_SUCCEEDED(rc)) rc=fsFileFlush(&manifest);
    fsFileClose(&manifest);
    if (R_FAILED(rc)) return rc;
    u8 expected[32],actual[32]; sha256ContextGetHash(&manifest_digest,expected);
    rc=hash_file(&ctx->sd,manifest_path,manifest_offset,actual);
    if (R_SUCCEEDED(rc) && memcmp(expected,actual,32)) rc=CLEAN_VERIFY;
    if (R_SUCCEEDED(rc)) rc=journal(ctx,"Retained-file copy and manifest verified.\n");
    return rc;
}
static Result compare_retained(Context *ctx) {
    size_t count=0; u64 bytes=0;
    Result rc=inventory(&ctx->save,&ctx->plan,ctx->check,&count,&bytes);
    if (R_FAILED(rc)) return rc;
    if (count!=ctx->count || bytes!=ctx->bytes) return CLEAN_CHANGED;
    for (size_t i=0;i<count;i++) {
        Entry *a=&ctx->entries[i],*b=&ctx->check[i];
        if (strcmp(a->path,b->path) || a->type!=b->type || a->size!=b->size) return CLEAN_CHANGED;
        if (a->type==1) {
            u8 hash[32]; rc=hash_file(&ctx->save,a->path,a->size,hash);
            if (R_FAILED(rc)) return rc;
            if (memcmp(hash,a->hash,32)) return CLEAN_VERIFY;
        }
        if (ctx->progress) ctx->progress("Checking retained save files",i,count);
    }
    return 0;
}
static Result preflight(void *v) {
    Context *ctx=v;
    fsFsClose(&ctx->save); ctx->save_active=false;
    Result rc=check_identity(&ctx->plan);
    if (R_FAILED(rc)) return rc;
    rc=open_save(&ctx->plan,false,&ctx->save);
    if (R_FAILED(rc)) return rc;
    ctx->save_active=true;
    CleanPlan now=ctx->plan; unsigned unknown=0;
    rc=find_targets(&ctx->save,&now,&unknown);
    if (R_FAILED(rc)) return rc;
    if (now.count!=ctx->plan.count) return CLEAN_CHANGED;
    for (size_t i=0;i<now.count;i++) {
        bool found=false; for (size_t j=0;j<ctx->plan.count;j++) if (!strcmp(now.names[i],ctx->plan.names[j])) found=true;
        if (!found) return CLEAN_CHANGED;
    }
    rc=compare_retained(ctx);
    if (R_SUCCEEDED(rc)) rc=journal(ctx,"Final target signatures and retained hashes match.\n");
    return rc;
}
static Result remove_tree(void *v, const char *path) {
    Context *ctx=v; char line[128];
    snprintf(line,sizeof(line),"Deleting only %s\n",path);
    Result rc=journal(ctx,line);
    if (R_FAILED(rc)) return rc;
    if (ctx->progress) ctx->progress("Removing recognized scratch folders",0,0);
    rc=fsFsDeleteDirectoryRecursively(&ctx->save,path);
    snprintf(line,sizeof(line),"Delete returned 0x%08x\n",rc);
    Result log_rc=journal(ctx,line);
    return R_FAILED(rc) ? rc : log_rc;
}
static Result commit(void *v) {
    Context *ctx=v;
    Result rc=journal(ctx,"Committing selected save once.\n");
    if (R_FAILED(rc)) return rc;
    rc=fsFsCommit(&ctx->save);
    char line[64]; snprintf(line,sizeof(line),"Commit returned 0x%08x\n",rc);
    Result log_rc=journal(ctx,line);
    if (R_FAILED(log_rc)) ctx->audit_error=log_rc;
    // Even if SD logging fails after a confirmed commit, verify the save now.
    return rc;
}
static Result verify(void *v) {
    Context *ctx=v;
    fsFsClose(&ctx->save); ctx->save_active=false;
    Result rc=open_save(&ctx->plan,true,&ctx->save);
    if (R_FAILED(rc)) return rc;
    ctx->save_active=true;
    for (size_t i=0;i<ctx->plan.count;i++) {
        char path[23]; clean_join(path,sizeof(path),"/",ctx->plan.names[i]);
        FsDirEntryType type;
        rc=fsFsGetEntryType(&ctx->save,path,&type);
        if (rc!=PATH_NOT_FOUND) return R_FAILED(rc) ? rc : CLEAN_VERIFY;
    }
    rc=compare_retained(ctx);
    if (R_SUCCEEDED(rc)) {
        Result log_rc=journal(ctx,"SUCCESS: fresh read-only mount confirms targets absent and retained bytes unchanged.\n");
        if (R_FAILED(log_rc)) ctx->audit_error=log_rc;
    }
    return rc;
}
CleanReport cleaner_clean(const CleanSave *save, char backup_path[FS_MAX_PATH], CleanProgress progress) {
    Context ctx={.plan=save->plan,.progress=progress};
    backup_path[0]=0;
    ctx.entries=calloc(MAX_ENTRIES,sizeof(Entry)); ctx.check=calloc(MAX_ENTRIES,sizeof(Entry));
    CleanReport result={.result=CLEAN_BOUNDS,.stage="memory allocation"};
    if (ctx.entries && ctx.check) {
        CleanOps ops={&ctx,backup,preflight,remove_tree,commit,verify};
        result=clean_execute(&ctx.plan,&ops);
    }
    if (ctx.journal_active) {
        char line[160]; snprintf(line,sizeof(line),"Final stage: %s; result=0x%08x; committed=%u verified=%u\n",result.stage,result.result,result.committed,result.verified);
        Result log_rc=journal(&ctx,line);
        if (R_FAILED(log_rc)) ctx.audit_error=log_rc;
        fsFileClose(&ctx.journal);
    }
    if (ctx.save_active) fsFsClose(&ctx.save);
    if (ctx.sd_active) fsFsClose(&ctx.sd);
    strcpy(backup_path,ctx.backup);
    free(ctx.entries); free(ctx.check);
    result.audit_error=ctx.audit_error;
    return result;
}
