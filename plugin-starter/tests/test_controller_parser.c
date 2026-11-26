#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../examples/plugin-control/command_parser.h"

static void accepts(const char* source, const char* expected_action, const char* expected_name) {
    char action[16], name[64];
    assert(p2ParseCommand(source, strlen(source), action, name));
    assert(!strcmp(action, expected_action)); assert(!strcmp(name, expected_name));
}
static void rejects_bytes(const char* source, size_t length) {
    char action[16], name[64];
    assert(!p2ParseCommand(source, length, action, name));
}
int main(void) {
    accepts("diag\r\n", "diag", ""); accepts("list\r\n", "list", ""); accepts("help", "help", ""); accepts("cancel", "cancel", "");
    accepts(" \tlaunch demo-1_2\r\n", "launch", "demo-1_2");
    accepts("stop demo", "stop", "demo"); accepts("restart demo", "restart", "demo");
    accepts("ping p2ping", "ping", "p2ping"); accepts("context p2ping", "context", "p2ping");
    const char* invalid[] = {"", " \n", "diag demo", "diag\nlist", "list demo", "launch", "stop ../x", "stop a.b", "launch x extra", "stop x\nstop x", "restartx x", "cancel extra", "ping third-party", "context third-party", "list\f", "stop a/b"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) rejects_bytes(invalid[i], strlen(invalid[i]));
    const char hidden[] = "stop p2ping\0 restart p2ping";
    rejects_bytes(hidden, sizeof(hidden) - 1);
    char longest[101] = "launch "; memset(longest + 7, 'a', 63); longest[70] = 0;
    accepts(longest, "launch", longest + 7);
    longest[70] = 'a'; longest[71] = 0; rejects_bytes(longest, 71);
    char oversized[101]; memset(oversized, ' ', sizeof(oversized)); rejects_bytes(oversized, sizeof(oversized));
    puts("controller parser behavioral tests passed");
    return 0;
}
