#include "core.h"
#include <stdio.h>
#include <string.h>
bool clean_application(uint64_t app) {
    return app == UINT64_C(0x010042d022aea000) || app == UINT64_C(0x0100ee5021c9e000);
}
bool clean_uid_is_active(const uint64_t uid[2], const uint64_t active[][2], size_t count) {
    if (!uid || !active || (!uid[0] && !uid[1]) || !count || count>CLEAN_MAX_USERS) return false;
    size_t matches=0;
    for (size_t i=0;i<count;i++) if (uid[0]==active[i][0] && uid[1]==active[i][1]) matches++;
    return matches==1;
}
bool clean_profile_check(const uint64_t save_uid[2], const uint64_t active[][2], size_t count, const uint64_t profile_record_uid[2], bool *record_differs) {
    if (!profile_record_uid || !record_differs || !clean_uid_is_active(save_uid,active,count)) return false;
    *record_differs=save_uid[0]!=profile_record_uid[0] || save_uid[1]!=profile_record_uid[1];
    return true;
}
bool clean_name(const char *name, char timestamp[14]) {
    if (!name || strlen(name) != 21 || memcmp(name, "NS2_GAP_", 8)) return false;
    for (unsigned i = 8; i < 21; i++) if (name[i] < '0' || name[i] > '9') return false;
    if (timestamp) { memcpy(timestamp, name + 8, 13); timestamp[13] = 0; }
    return true;
}
bool clean_component(const char *name) {
    if (!name || !*name || !strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p < 32 || *p == 127 || *p == '/' || *p == '\\') return false;
    return true;
}
bool clean_join(char *out, size_t capacity, const char *parent, const char *name) {
    if (!capacity || !parent || parent[0] != '/' || !clean_component(name)) return false;
    int n = snprintf(out, capacity, "%s%s%s", parent, strcmp(parent, "/") ? "/" : "", name);
    return n >= 0 && (size_t)n < capacity;
}
bool clean_plan_valid(const CleanPlan *p) {
    if (!p || !clean_application(p->application) || !p->save_id || (!p->uid[0] && !p->uid[1]) ||
        p->space != 1 || p->type != 1 || p->rank || p->index || !p->count || p->count > CLEAN_MAX_TARGETS) return false;
    for (size_t i = 0; i < p->count; i++) {
        if (!memchr(p->names[i], 0, CLEAN_NAME_SIZE) || !clean_name(p->names[i], NULL)) return false;
        for (size_t j = 0; j < i; j++) if (!strcmp(p->names[i], p->names[j])) return false;
    }
    return true;
}
static bool only_child(CleanReadDirectory read_directory, void *context, const char *path, const char *name, uint8_t type, int64_t size) {
    CleanNode child; size_t count=0;
    if (read_directory(context,path,&child,1,&count)) return false;
    return count==1 && !strcmp(child.name,name) && child.type==type && (!type || child.size==size);
}
bool clean_recognized(const char *name, CleanReadDirectory read_directory, void *context) {
    char stamp[14],path[CLEAN_PATH_SIZE],next[CLEAN_PATH_SIZE];
    if (!read_directory || !clean_name(name,stamp) || !clean_join(path,sizeof(path),"/",name)) return false;
    CleanNode children[4]; size_t count=0;
    if (read_directory(context,path,children,4,&count) || count<3 || count>4) return false;
    char s0[20],s1[20],s2[20];
    snprintf(s0,sizeof(s0),"S0_%s",stamp); snprintf(s1,sizeof(s1),"S1_%s",stamp); snprintf(s2,sizeof(s2),"S2_%s",stamp);
    unsigned flags=0;
    for (size_t i=0;i<count;i++) {
        CleanNode *e=&children[i];
        if (!memchr(e->name,0,sizeof(e->name)) || !clean_component(e->name)) return false;
        if (!strcmp(e->name,s0) && e->type==0 && !(flags&1)) flags|=1;
        else if (!strcmp(e->name,"canary") && e->type==0 && !(flags&2)) flags|=2;
        else if (!strcmp(e->name,"commit_data") && e->type==1 && e->size>0 && e->size<=INT64_C(512)*1024*1024 && !(flags&4)) flags|=4;
        else if (!strcmp(e->name,"selected") && e->type==0 && !(flags&8)) {
            flags|=8;
            if (!clean_join(next,sizeof(next),path,"selected")) return false;
            CleanNode empty; size_t n=0;
            if (read_directory(context,next,&empty,1,&n) || n) return false;
        } else return false;
    }
    if ((flags&7)!=7) return false;
    if (!clean_join(next,sizeof(next),path,s0) || !only_child(read_directory,context,next,s1,0,0)) return false;
    if (!clean_join(path,sizeof(path),next,s1) || !only_child(read_directory,context,path,s2,0,0)) return false;
    if (!clean_join(next,sizeof(next),path,s2) || !only_child(read_directory,context,next,"SPAN_MARKER",1,1)) return false;
    if (!clean_join(path,sizeof(path),"/",name) || !clean_join(next,sizeof(next),path,"canary")) return false;
    strcpy(path,next);
    for (unsigned depth=0;depth<64;depth++) {
        CleanNode child; size_t n=0;
        if (read_directory(context,path,&child,1,&n) || n>1) return false;
        if (!n) return true;
        if (child.type!=0 || !memchr(child.name,0,sizeof(child.name)) || strcmp(child.name,"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA")) return false;
        // Recognize the known canary boundary without opening an overlong path.
        if (!clean_join(next,sizeof(next),path,child.name)) return true;
        strcpy(path,next);
    }
    return false;
}
CleanReport clean_execute(const CleanPlan *plan, const CleanOps *ops) {
    CleanReport r = {.result=CLEAN_BAD_PLAN, .stage="validation"};
    if (!clean_plan_valid(plan) || !ops || !ops->backup || !ops->preflight || !ops->remove_tree || !ops->commit || !ops->verify) return r;
    r.stage = "preservation copy";
    if ((r.result = ops->backup(ops->context))) return r;
    r.backup_verified = true;
    r.stage = "final save check";
    if ((r.result = ops->preflight(ops->context))) return r;
    r.stage = "scratch deletion";
    for (size_t i = 0; i < plan->count; i++) {
        char target[23];
        if (!clean_join(target, sizeof(target), "/", plan->names[i])) { r.result=CLEAN_BAD_PLAN; return r; }
        r.write_started = true;
        if ((r.result = ops->remove_tree(ops->context, target))) return r;
        r.deleted++;
    }
    r.stage = "save commit";
    if ((r.result = ops->commit(ops->context))) return r;
    r.committed = true;
    r.stage = "read-only verification";
    if ((r.result = ops->verify(ops->context))) return r;
    r.verified = true;
    r.stage = "complete";
    return r;
}
