#!/usr/bin/env python3
"""Exercise the actual transaction function bodies with host-side IPC/SD mocks.

These tests check decisions and failure ordering, not kernel or hardware behavior.
The package fixture uses a deterministic mock digest, not a crypto implementation.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(path, start, end):
    text = (ROOT / path).read_text()
    return text[text.index(start):text.index(end, text.index(start))]


COMMON = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include <iostream>
using u8=uint8_t; using u32=uint32_t; using u64=uint64_t;
using Result=u32; using Handle=u32;
constexpr Handle INVALID_HANDLE=0, CUR_PROCESS_HANDLE=999;
constexpr Result Invalid=1, Module_Libnx=1, LibnxError_AlreadyInitialized=2;
#define MAKERESULT(m,d) ((m)*100+(d))
#define R_FAILED(r) ((r)!=0)
#define R_SUCCEEDED(r) ((r)==0)
struct Service { Handle handle=0; };
void serviceCreate(Service* s, Handle h) { s->handle=h; }
void svcSleepThread(u64) {}
void svcCloseHandle(Handle) {}
'''

RESTART = r'''
enum class RestartPhase { Idle, HoldOldHen, LaunchCandidate, ConnectCandidate,
 ConfigureCandidate, WarmCandidate, StageRo, SwitchRo, StopOldHen, ActivateNewHen,
 CommitRo, RunningReplacement, RestoreOld, Unresolved };
struct RestartReport {
 RestartPhase phase=RestartPhase::Idle; Result result=0,recovery=0;
 u64 previous_pid=0,new_pid=0,generation=1;
 u32 rollback_available=0,old_hen_stopped=0,old_ro_stopped=0;
};
enum class LaunchPhase { Completed, Failure };
struct LaunchReport { LaunchPhase phase=LaunchPhase::Failure; u64 pid=0; };
enum class Fault { None, LaunchRestored, LaunchUnknown, Stage, Switch, StopReply,
 WaitReply, OldStillAlive, Activate, Commit, Rollback, Terminate, Resume };
Fault fault=Fault::None;
int resumes=0,terminates=0,rollbacks=0,activations=0,stops=0;
constexpr u32 HEN_PROTOCOL_VERSION=7;
constexpr u32 RoCmd_StageHenControl=1,RoCmd_SwitchHenControl=2,
 RoCmd_FinalizeHenControl=3,RoCmd_RollbackHenControl=4;
const char *CandidateMain="main",*CandidatePackage="package",*PackageSd="sd";
struct HenStatus { u64 process_id; u32 protocol_version; Result initialization_result; };
struct Launcher {
 bool locked=false;
 bool available() { return !locked; }
 Result launch(const char*,const char*,const char*,LaunchReport* r,const u8*,u64) {
  if(fault==Fault::LaunchUnknown) { locked=true; return 4; }
  if(fault==Fault::LaunchRestored) return 4;
  r->pid=20; r->phase=LaunchPhase::Completed; return 0;
 }
};
Result henGetStatus(Service* s,HenStatus* h) { *h={s->handle,7,0}; return 0; }
Result henQuerySupervisorQuiescence(Service* s,u32* v) { *v=s->handle==10?1:2; return 0; }
Result henHoldGameArm(Service*) { return 0; }
Result henWarmupPaused(Service*) { return 0; }
Result henStopIfHeld(Service*) { ++stops; return fault==Fault::StopReply?5:0; }
Result henActivatePaused(Service*) { ++activations; return fault==Fault::Activate?6:0; }
Result henResumeGameArm(Service*) { ++resumes; return fault==Fault::Resume?7:0; }
Service pm;
Service* pmdmntGetServiceSession() { return &pm; }
Service* pmshellGetServiceSession() { return &pm; }
Result pmdmntGetProgramId(u64*,u64) {
 if(fault==Fault::WaitReply) return 9;
 if(fault==Fault::OldStillAlive) return 0;
 return MAKERESULT(15,1);
}
Result pmshellTerminateProcess(u64) { ++terminates; return fault==Fault::Terminate?8:0; }
struct Controller {
 Launcher launcher_;
 bool unresolved_=false,owns_active_supervisor_=false;
 Handle active_supervisor_=10,supervisor_control_client_=30;
 u64 generation_=1;
 struct { Handle monitor_session=1,shell_session=2; } escrow_;
 Result replaceHen(RestartReport*,const u8*,u64);
 Result connectMigratedHen(Handle* h) { *h=20; return 0; }
 Result configureNewHen(Handle,Handle* h) { *h=20; return 0; }
 Result stageRo(Handle,u64) { return fault==Fault::Stage?11:0; }
 Result roCommand(u32 c,u64) {
  if(c==RoCmd_RollbackHenControl) { ++rollbacks; return fault==Fault::Rollback?13:0; }
  if(c==RoCmd_SwitchHenControl && (fault==Fault::Switch || fault==Fault::Rollback ||
      fault==Fault::Terminate || fault==Fault::Resume)) return 12;
  if(c==RoCmd_FinalizeHenControl && fault==Fault::Commit) return 14;
  return 0;
 }
};
'''

RESTART_TESTS = r'''
int main() {
 u8 hash[32]={1};
 for(Fault f: {Fault::None,Fault::LaunchRestored,Fault::LaunchUnknown,Fault::Stage,
 Fault::Switch,Fault::StopReply,Fault::WaitReply,Fault::OldStillAlive,
 Fault::Activate,Fault::Commit,Fault::Rollback,Fault::Terminate,Fault::Resume}) {
  fault=f; resumes=terminates=rollbacks=activations=stops=0;
  Controller c; RestartReport r;
  Result rc=c.replaceHen(&r,hash,20000);
  if(f==Fault::None) {
   assert(rc==0 && r.phase==RestartPhase::RunningReplacement && r.generation==2);
   assert(r.old_hen_stopped && !resumes && !terminates && activations==1);
  } else if(f==Fault::LaunchUnknown || f==Fault::Stage || f==Fault::StopReply || f==Fault::WaitReply ||
    f==Fault::OldStillAlive || f==Fault::Activate || f==Fault::Commit ||
    f==Fault::Rollback || f==Fault::Terminate) {
   assert(rc && c.unresolved_ && r.phase==RestartPhase::Unresolved);
   assert(!r.rollback_available && !resumes);
   if(f!=Fault::Rollback && f!=Fault::Terminate) assert(!terminates && !rollbacks);
   if(f==Fault::StopReply || f==Fault::WaitReply || f==Fault::OldStillAlive)
    assert(!activations);
  } else {
   assert(rc && resumes==1);
   if(f==Fault::Rollback || f==Fault::Terminate || f==Fault::Resume)
    assert(c.unresolved_ && r.phase==RestartPhase::Unresolved);
   else assert(!c.unresolved_ && r.phase==RestartPhase::RestoreOld);
  }
 }
 std::cout << "Actual HEN transaction: 13 success/failure scenarios PASS\n";
}
'''

QUEUE = r'''
using Mutex=int; void mutexLock(Mutex*) {} void mutexUnlock(Mutex*) {}
u64 armGetSystemTick() { return 1; }
constexpr Result Busy=2;
constexpr u64 DeferredHenConfirmation=42;
enum class DeferredKind { DryRun=1,ReplaceHen=2 };
enum class DeferredPhase { Idle,WaitingForHostExit,WaitingForHenIdle,HenExecuting };
bool pending(DeferredPhase p) { return p==DeferredPhase::WaitingForHostExit; }
bool executing(DeferredPhase p) { return p==DeferredPhase::HenExecuting; }
struct DeferredRequest { u64 confirmation,host_pid; };
struct DeferredHenPackageRequest { u64 confirmation,host_pid,main_size; u8 sha256[32]; };
struct DeferredReport { u32 phase=0,ticket=0,kind=0; u64 host_pid=0; };
Service pm;
Service* pmdmntGetServiceSession() { return &pm; }
Result pmdmntGetProgramId(u64*,u64) { return 0; }
struct Controller {
 u64 size=20000; u8 digest[32]={1};
 Result preflightHen(u8* d,u64* s) { std::memcpy(d,digest,32); *s=size; return 0; }
};
struct DeferredMonitor {
 bool started_=true,live_attempted_=false;
 u32 live_attempts_=0,consecutive_safe_=0; static constexpr u32 MaxLive=3;
 u64 staged_size_=0,start_ticks_=0; u8 staged_digest_[32]={};
 Mutex lock_=0,pm_ipc_lock_=0,fs_ipc_lock_=0;
 Handle monitor_session_=1; DeferredReport report_; Controller controller_;
 Result queueHenPackage(const DeferredHenPackageRequest&,DeferredReport*);
 Result queue(const DeferredRequest&,DeferredKind,DeferredReport*,
              const DeferredHenPackageRequest* =nullptr);
};
'''

QUEUE_TESTS = r'''
int main() {
 DeferredHenPackageRequest r={42,10,20000,{1}}; DeferredReport out;
 { DeferredMonitor m; assert(!m.queueHenPackage(r,&out) && out.ticket==1); }
 { DeferredMonitor m; m.controller_.digest[0]=2;
   assert(m.queueHenPackage(r,&out) && m.report_.ticket==0); }
 { DeferredMonitor m; m.controller_.size++;
   assert(m.queueHenPackage(r,&out) && m.report_.ticket==0); }
 { DeferredMonitor m; r.confirmation=0; assert(m.queueHenPackage(r,&out)); r.confirmation=42; }
 { DeferredMonitor m; r.main_size=0; assert(m.queueHenPackage(r,&out)); r.main_size=20000; }
 { DeferredMonitor m; r.host_pid=0; assert(m.queueHenPackage(r,&out)); r.host_pid=10; }
 { DeferredMonitor m; m.live_attempted_=true; assert(m.queueHenPackage(r,&out)); }
 { DeferredMonitor m; m.live_attempts_=3; assert(m.queueHenPackage(r,&out)); }
 std::cout << "Actual digest-bound queue: 8 scenarios PASS\n";
}
'''

PACKAGE = r'''
constexpr Result InvalidPackage=1;
constexpr size_t CopyChunk=4096;
u64 align32(u64 v) { return (v+31)&~u64(31); }
struct SignedPolicy { static constexpr size_t Capacity=8192; u8 bytes[Capacity]={}; u32 size=8; };
std::vector<u8> source(20000,3),output;
bool short_read=false,write_error=false,finished=false;
// Mock the digest so these tests isolate byte/size binding and failure ordering.
struct Sha256Context { u8 v=0; };
void sha256ContextCreate(Sha256Context* s) { s->v=0; }
void sha256ContextUpdate(Sha256Context* s,const void* b,size_t n) {
 for(size_t i=0;i<n;i++) s->v=static_cast<u8>(s->v*33+static_cast<const u8*>(b)[i]);
}
void sha256ContextGetHash(Sha256Context* s,void* d) { std::memset(d,s->v,32); }
struct SdReader {
 size_t at=0;
 Result begin(Handle,const char*,u64* n) { *n=source.size(); return 0; }
 Result read(void* b,size_t n,u64* received) {
  if(short_read && n) --n;
  std::memcpy(b,source.data()+at,n); at+=n; *received=n; return 0;
 }
 void close() {}
};
struct SdWriter {
 Result begin(Handle,const char*,u64) { output.clear(); finished=false; return 0; }
 Result write(const void* b,size_t n) {
  if(write_error) return 2;
  auto* p=static_cast<const u8*>(b); output.insert(output.end(),p,p+n); return 0;
 }
 Result finish() { finished=true; return 0; }
};
'''

PACKAGE_TESTS = r'''
int main() {
 u8 hash[32]; Sha256Context s; sha256ContextCreate(&s);
 sha256ContextUpdate(&s,source.data(),source.size()); sha256ContextGetHash(&s,hash);
 SignedPolicy p;
 assert(!writeExeFsPackage(1,"main","pkg",p,hash,source.size()) && finished);
 const size_t header=16+2*24+sizeof("main")+sizeof("main.npdm");
 assert(!std::memcmp(output.data()+header,source.data(),source.size()));
 source[100]++; finished=false;
 assert(writeExeFsPackage(1,"main","pkg",p,hash,source.size()) && !finished);
 source[100]--; finished=false;
 assert(writeExeFsPackage(1,"main","pkg",p,hash,source.size()+1) && !finished);
 short_read=true;
 assert(writeExeFsPackage(1,"main","pkg",p,hash,source.size()) && !finished);
 short_read=false; write_error=true;
 assert(writeExeFsPackage(1,"main","pkg",p,hash,source.size()) && !finished);
 write_error=false;
 assert(!writeExeFsPackage(1,"main","pkg",p,nullptr,0));
 std::cout << "Actual package writer: 6 byte-binding/I/O scenarios PASS\n";
}
'''


def main():
    restart = function('hotupdate-supervisor/source/restart_controller.cpp',
                       'Result Controller::replaceHen(', '} // namespace hotupdate')
    queue = function('hotupdate-supervisor/source/deferred_monitor.cpp',
                     'Result DeferredMonitor::queueHenPackage(', 'Result DeferredMonitor::status(')
    package = (ROOT / 'common/policy_package.cpp').read_text().split('Result writeExeFsPackage(', 1)[1]
    cases = [('restart', RESTART + restart + RESTART_TESTS),
             ('queue', QUEUE + queue + QUEUE_TESTS),
             ('package', PACKAGE + 'Result writeExeFsPackage(' + package + PACKAGE_TESTS)]
    with tempfile.TemporaryDirectory(prefix='hen-updater-tests-') as temp:
        for name, body in cases:
            src = Path(temp) / (name + '.cpp')
            binary = Path(temp) / name
            src.write_text(COMMON + body)
            command = shlex.split(os.environ.get('CXX', 'c++'))
            subprocess.run(command + ['-std=c++20', '-Wall', '-Wextra', '-Werror',
                           '-fsanitize=address,undefined', str(src), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
