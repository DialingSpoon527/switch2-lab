from pathlib import Path
import subprocess
import tempfile
import re

root = Path(__file__).resolve().parent
source = root.parent / 'hen-updater'
text = (source / 'hen/source/server.cpp').read_text()
start = text.index('bool HenServer::pluginWorkIdleForReplacement() const {')
end = text.index('\nvoid HenServer::serveSupervisorCommand', start)
body = text[start:end]
fixture = r'''
#include <cassert>
#include <cstring>
#include <cstdint>
#include "managed_registry.hpp"
using u32=std::uint32_t;
struct HenServer {
    struct Background {
        bool ready=true, gate=false;
        bool canQueue() const { return ready; }
        bool gateOwned() const { return gate; }
    } background_;
    struct Grace { bool busy=false; bool occupied() const { return busy; } } graceful_;
    bool restart_pending_=false;
    pluginmanager::Registry managed_;
    bool pluginWorkIdleForReplacement() const;
};
'''
checks = r'''
int main() {
    HenServer s;
    assert(s.pluginWorkIdleForReplacement());
    assert(s.managed_.track("hotupdate-supervisor", 1, 0, pluginmanager::BootPlugin));
    assert(s.pluginWorkIdleForReplacement());
    s.background_.ready=false; assert(!s.pluginWorkIdleForReplacement()); s.background_.ready=true;
    s.background_.gate=true; assert(!s.pluginWorkIdleForReplacement()); s.background_.gate=false;
    s.graceful_.busy=true; assert(!s.pluginWorkIdleForReplacement()); s.graceful_.busy=false;
    s.restart_pending_=true; assert(!s.pluginWorkIdleForReplacement()); s.restart_pending_=false;
    assert(s.managed_.track("p2ping", 2, 1, pluginmanager::OnDemand));
    assert(!s.pluginWorkIdleForReplacement());
    std::uint64_t pid=0; assert(s.managed_.beginStop("p2ping", &pid));
    assert(!s.pluginWorkIdleForReplacement());
    assert(s.managed_.stopFailed("p2ping", pid, 7));
    assert(!s.pluginWorkIdleForReplacement());
    assert(s.managed_.exited("p2ping", pid));
    assert(s.pluginWorkIdleForReplacement());
    assert(s.managed_.track("p2resident", 3, 0, pluginmanager::BootPlugin));
    assert(!s.pluginWorkIdleForReplacement());
    assert(s.managed_.exited("p2resident", 3));
    assert(s.pluginWorkIdleForReplacement());
}
'''
control = (source / 'common/control.h').read_text()
enum = control[control.index('enum HenControlCommand'):control.index('};')]
ids = re.findall(r'(HenControlCommand_\w+)\s*=\s*(\d+)', enum)
assert len({value for _, value in ids}) == len(ids), 'Private command collision'
with tempfile.TemporaryDirectory() as directory:
    p = Path(directory)
    (p / 'test.cpp').write_text(fixture + body + checks)
    subprocess.run(['g++', '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I' + str(source / 'hen/source'),
                    str(p / 'test.cpp'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
print('Actual merged replacement coordination: idle, supervisor, active work, running/stopping/failed/stopped plugins PASS')
print('Private bootstrap command IDs are unique PASS')
