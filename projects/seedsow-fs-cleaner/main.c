#include "native.h"
#include <stdio.h>
#include <string.h>
static PadState pad;
static CleanSave saves[CLEAN_MAX_SAVES];
static size_t save_count, selected;
static bool session_blocked;
static u64 last_progress;

static const char *game_name(const CleanPlan *plan) {
    return plan->application==UINT64_C(0x010042d022aea000) ? "Seedsow Demo" : "Seedsow full game";
}
static void heading(void) {
    consoleClear();
    printf("Seedsow FS cleanup 0.1.2\n\n");
}
static void progress(const char *phase, size_t at, size_t count) {
    u64 now=armGetSystemTick();
    if (last_progress && armTicksToNs(now-last_progress)<100000000ull) return;
    last_progress=now;
    heading(); printf("%s\n",phase);
    if (count) printf("%zu / %zu\n",at,count);
    printf("\nPlease leave the app open until it finishes.\n");
    consoleUpdate(NULL);
}
static Result scan(void) {
    heading(); printf("Checking Seedsow saves. No save changes.\n"); consoleUpdate(NULL);
    selected=0;
    Result rc=cleaner_scan(saves,&save_count);
    if (R_SUCCEEDED(rc)) {
        for (size_t i=0;i<save_count;i++) {
            if (R_SUCCEEDED(saves[i].scan_result) && saves[i].plan.count) { selected=i; break; }
        }
    }
    return rc;
}
static void show_list(Result scan_result) {
    heading();
    printf("Removes recognized FS-attempt scratch folders.\n");
    printf("Launcher programs and game saves are kept.\n");
    printf("SD copy is verified before any save deletion.\n\n");
    printf("Cleanup applies to the selected profile only.\n\n");
    if (R_FAILED(scan_result)) { printf("Scan failed: 0x%08x. No cleanup available.\n",scan_result); return; }
    if (!save_count) printf("No supported Seedsow account saves found.\n");
    size_t start=selected>=8 ? selected-7 : 0;
    for (size_t i=start;i<save_count && i<start+8;i++) {
        CleanSave *s=&saves[i];
        printf("%s %-20.32s %s\n",i==selected ? ">" : " ",s->nickname,game_name(&s->plan));
        if (R_FAILED(s->scan_result)) printf("    Cannot inspect (0x%08x).\n",s->scan_result);
        else printf("    %zu recognized scratch folder(s)%s\n",s->plan.count,s->unrecognized ? "; unknown folders left alone" : "");
    }
    if (session_blocked) printf("\nA write outcome needs inspection. Cleanup disabled.\n");
    printf("\nUp/Down: profile  A: review profile  X: rescan\n+: exit\n");
}
static void confirmation(const CleanSave *save) {
    heading();
    printf("Profile: %s\n%s\n\n",save->nickname,game_name(&save->plan));
    if (save->profile_data_mismatch) printf("Profile record differs; active account/save match checked.\n\n");
    printf("Remove ONLY these recognized scratch folders:\n");
    for (size_t i=0;i<save->plan.count;i++) printf("  /%s\n",save->plan.names[i]);
    printf("\nFiles outside them will be copied and checked on SD.\n");
    printf("Scratch contents themselves are not backed up.\n");
    printf("\nThis writes to your Seedsow entrypoint save.\n");
    printf("A failure could corrupt it or break that entrypoint.\n");
    printf("The SD copy is not a guaranteed recovery method.\n");
    printf("Close other save tools before continuing.\n");
    printf("\nHold L + R, then press A to clean this profile.\n");
    printf("B: cancel  +: exit\n");
}
static void show_result(CleanReport report, const char *backup_path, const CleanSave *save) {
    heading();
    printf("Profile: %s\n%s\n\n",save->nickname,game_name(&save->plan));
    if (report.verified) {
        printf("Cleanup verified.\n\n");
        printf("Removed %zu scratch folder(s).\n",report.deleted);
        printf("Retained file hashes match the originals.\n");
        printf("\nYou can now try your Checkpoint/JKSV backup.\n");
        printf("Choose this same profile in the backup app.\n");
        printf("Replacing a save is still a separate operation.\n");
    } else {
        printf("Stopped during: %s\nResult: 0x%08x\n\n",report.stage,report.result);
        if (!report.write_started) printf("No save deletion or commit was issued.\n");
        else printf("Do not retry cleanup until the save is inspected.\n");
        printf("Backup verified: %s\nCommit confirmed: %s\n",report.backup_verified ? "yes" : "no",report.committed ? "yes" : "no");
    }
    if (*backup_path) printf("\nSD copy and local operation record:\n%s\n",backup_path);
    if (report.audit_error) printf("\nSD operation log could not finish: 0x%08x\n",report.audit_error);
    printf("\nB: return to list  +: exit\n");
}
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    consoleInit(NULL); padConfigureInput(1,HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
    heading(); printf("Opening kaiHEN homebrew filesystem access...\n"); consoleUpdate(NULL);
    u32 protocol=0; Result rc=cleaner_initialize(&protocol);
    if (R_FAILED(rc)) {
        heading(); printf("kaiHEN filesystem access unavailable: 0x%08x\n",rc);
        printf("Launch after kaiHEN is running.\nNo saves changed.\n\n+: exit\n");
        while (appletMainLoop()) { padUpdate(&pad); if (padGetButtonsDown(&pad)&HidNpadButton_Plus) break; consoleUpdate(NULL); }
        cleaner_exit(); consoleExit(NULL); return 1;
    }
    Result scan_result=scan();
    enum { List, Confirm, ResultScreen } screen=List;
    bool redraw=true;
    CleanReport report={0}; char backup_path[FS_MAX_PATH]={0};
    while (appletMainLoop()) {
        padUpdate(&pad); u64 down=padGetButtonsDown(&pad),held=padGetButtons(&pad);
        if (down&HidNpadButton_Plus) break;
        if (screen==List) {
            if ((down&HidNpadButton_Down) && save_count) { selected=(selected+1)%save_count; redraw=true; }
            if ((down&HidNpadButton_Up) && save_count) { selected=(selected+save_count-1)%save_count; redraw=true; }
            if (down&HidNpadButton_X) { scan_result=scan(); redraw=true; }
            if ((down&HidNpadButton_A) && R_SUCCEEDED(scan_result) && save_count && !session_blocked &&
                R_SUCCEEDED(saves[selected].scan_result) && saves[selected].plan.count) { screen=Confirm; redraw=true; }
        } else if (screen==Confirm) {
            if (down&HidNpadButton_B) { screen=List; redraw=true; }
            else if ((down&HidNpadButton_A) && (held&HidNpadButton_L) && (held&HidNpadButton_R)) {
                last_progress=0;
                report=cleaner_clean(&saves[selected],backup_path,progress);
                if (report.write_started && !report.verified) session_blocked=true;
                screen=ResultScreen; redraw=true;
            }
        } else if (down&HidNpadButton_B) { scan_result=scan(); screen=List; redraw=true; }
        if (redraw) {
            if (screen==List) show_list(scan_result);
            else if (screen==Confirm) confirmation(&saves[selected]);
            else show_result(report,backup_path,&saves[selected]);
            redraw=false;
        }
        consoleUpdate(NULL);
    }
    cleaner_exit(); consoleExit(NULL); return 0;
}
