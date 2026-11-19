#include "core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct { int step, fail, backups, preflights, deletes, commits, verifies; } Mock;
static uint32_t call(Mock *m) { return ++m->step == m->fail ? 123 : 0; }
static uint32_t backup(void *v) { Mock *m=v; m->backups++; return call(m); }
static uint32_t preflight(void *v) { Mock *m=v; assert(m->backups==1); m->preflights++; return call(m); }
static uint32_t remove_tree(void *v, const char *path) {
    Mock *m=v; assert(m->backups==1 && m->preflights==1 && !m->commits);
    assert(!strcmp(path,"/NS2_GAP_1700000000001") || !strcmp(path,"/NS2_GAP_1700000000002"));
    m->deletes++; return call(m);
}
static uint32_t commit(void *v) { Mock *m=v; assert(m->deletes==2); m->commits++; return call(m); }
static uint32_t verify(void *v) { Mock *m=v; assert(m->commits==1); m->verifies++; return call(m); }
typedef struct { int variant; unsigned calls; bool reached_boundary; } Tree;
static uint32_t read_tree(void *v, const char *path, CleanNode *nodes, size_t capacity, size_t *count) {
    Tree *t=v; t->calls++;
    assert(strlen(path)<769); *count=0;
    if (t->variant==1) return 99;
    const char *root="/NS2_GAP_1700000000001";
    assert(!strncmp(path,root,strlen(root)));
    const char *sub=path+strlen(root);
    memset(nodes,0,capacity*sizeof(*nodes));
    if (!*sub) {
        assert(capacity==4); *count=4;
        strcpy(nodes[0].name,"S0_1700000000001");
        strcpy(nodes[1].name,"selected");
        strcpy(nodes[2].name,"canary");
        strcpy(nodes[3].name,"commit_data"); nodes[3].type=1; nodes[3].size=8388608;
        if (t->variant==2) strcpy(nodes[1].name,"my-programs");
        if (t->variant==3) strcpy(nodes[0].name,"S0_1700000000002");
        if (t->variant==4) nodes[3].size=0;
        if (t->variant==5) nodes[2].type=1;
    } else if (!strcmp(sub,"/selected")) {
        if (t->variant==6) { *count=1; strcpy(nodes[0].name,"important.txt");nodes[0].type=1; }
    } else if (!strcmp(sub,"/S0_1700000000001")) { *count=1;strcpy(nodes[0].name,"S1_1700000000001"); }
    else if (!strcmp(sub,"/S0_1700000000001/S1_1700000000001")) { *count=1;strcpy(nodes[0].name,"S2_1700000000001"); }
    else if (!strcmp(sub,"/S0_1700000000001/S1_1700000000001/S2_1700000000001")) {
        *count=1;strcpy(nodes[0].name,"SPAN_MARKER");nodes[0].type=1;nodes[0].size=t->variant==7 ? 2 : 1;
    } else if (!strncmp(sub,"/canary",7)) {
        if (t->variant==8) return 99;
        if (t->variant==9) return 0;
        *count=1;strcpy(nodes[0].name,"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
        if (t->variant==10) strcpy(nodes[0].name,"user-data");
        if (t->variant==11) nodes[0].type=1;
        if (strlen(path)>735) t->reached_boundary=true;
    } else assert(0);
    return 0;
}
int main(void) {
    const uint64_t uid[2]={1,2}, absent[2]={9,9}, zero[2]={0,0};
    uint64_t active[CLEAN_MAX_USERS][2]={{3,4},{1,2}};
    assert(clean_uid_is_active(uid,active,CLEAN_MAX_USERS));
    assert(!clean_uid_is_active(absent,active,CLEAN_MAX_USERS));
    assert(!clean_uid_is_active(zero,active,CLEAN_MAX_USERS));
    assert(!clean_uid_is_active(uid,active,CLEAN_MAX_USERS+1));
    bool record_differs=true;
    assert(clean_profile_check(uid,active,CLEAN_MAX_USERS,uid,&record_differs) && !record_differs);
    const uint64_t inconsistent_record[2]={100,200};
    assert(clean_profile_check(uid,active,CLEAN_MAX_USERS,inconsistent_record,&record_differs) && record_differs);
    assert(clean_profile_check(uid,active,CLEAN_MAX_USERS,active[0],&record_differs) && record_differs);
    assert(!clean_profile_check(absent,active,CLEAN_MAX_USERS,uid,&record_differs));
    assert(!clean_profile_check(uid,active,CLEAN_MAX_USERS,NULL,&record_differs));
    active[2][0]=1; active[2][1]=2;
    assert(!clean_uid_is_active(uid,active,CLEAN_MAX_USERS));
    assert(!clean_profile_check(uid,active,CLEAN_MAX_USERS,inconsistent_record,&record_differs));
    CleanPlan plan={.application=0x010042d022aea000,.save_id=0x123,.uid={1,2},.space=1,.type=1,.count=2};
    strcpy(plan.names[0],"NS2_GAP_1700000000001"); strcpy(plan.names[1],"NS2_GAP_1700000000002");
    assert(clean_plan_valid(&plan));
    assert(clean_application(UINT64_C(0x0100ee5021c9e000)));
    for (int variant=0;variant<12;variant++) {
        Tree tree={.variant=variant};
        bool accepted=clean_recognized(plan.names[0],read_tree,&tree);
        assert(accepted==(variant==0 || variant==9));
        if (variant==0) assert(tree.reached_boundary);
        assert(tree.calls<64);
    }
    const char *bad[]={"NS2_GAP_", "NS2_GAP_170000000000", "NS2_GAP_17000000000015", "NS2_GAP_170000000000x", "../NS2_GAP_1700000000001", "NS2_GAP_1700000000001/..", "/NS2_GAP_1700000000001", "NS2_GAP_1700000000001.bak"};
    for (size_t i=0;i<sizeof(bad)/sizeof(*bad);i++) assert(!clean_name(bad[i],NULL));
    char stamp[14], path[769]; assert(clean_name(plan.names[0],stamp) && !strcmp(stamp,"1700000000001"));
    assert(clean_join(path,sizeof(path),"/","save.dat") && !strcmp(path,"/save.dat"));
    assert(!clean_join(path,sizeof(path),"/","..")); assert(!clean_join(path,sizeof(path),"/","x/y"));
    assert(!clean_join(path,sizeof(path),"/","x\\y")); assert(!clean_join(path,sizeof(path),"/","x\ny"));
    char longname[769]; memset(longname,'A',767);longname[767]=0;
    assert(clean_join(path,sizeof(path),"/",longname));longname[767]='A';longname[768]=0;
    assert(!clean_join(path,sizeof(path),"/",longname));
    for (int fail=0;fail<=6;fail++) {
        Mock m={.fail=fail}; CleanOps ops={&m,backup,preflight,remove_tree,commit,verify};
        CleanReport r=clean_execute(&plan,&ops);
        if (!fail) { assert(!r.result && r.verified && r.committed && r.deleted==2 && m.step==6); }
        else {
            assert(r.result==123 && !r.verified && m.step==fail);
            if (fail<=2) assert(!r.write_started && !m.deletes && !m.commits);
            if (fail==3 || fail==4) assert(!m.commits && !m.verifies);
            if (fail==5) assert(!r.committed && !m.verifies);
            if (fail==6) assert(r.committed);
        }
    }
    for (unsigned badfield=0;badfield<9;badfield++) {
        CleanPlan p=plan;
        switch (badfield) {
            case 0:p.application=1;break;case 1:p.space=0;break;case 2:p.type=0;break;
            case 3:p.rank=1;break;case 4:p.index=1;break;case 5:p.uid[0]=p.uid[1]=0;break;
            case 6:p.count=17;break;case 7:strcpy(p.names[1],p.names[0]);break;case 8:p.save_id=0;break;
        }
        Mock m={0};CleanOps ops={&m,backup,preflight,remove_tree,commit,verify};
        CleanReport r=clean_execute(&p,&ops);assert(r.result==CLEAN_BAD_PLAN && !m.step);
    }
    puts("PASS: active user/save identity with inconsistent profile records; inactive and duplicate users rejected; scratch signatures, path bounds, and fail-stop transaction stages; no retries");
}
