// SPDX-License-Identifier: GPL-2.0-only
// A bounded, allocation-free registry. Kept independent of the Horizon SDK
// so the launch/stop/restart state transitions can be tested on the PC.
#pragma once
#include <cstdint>
#include <cstring>

namespace pluginmanager {
constexpr unsigned NameCapacity = 64;
constexpr unsigned RegistryCapacity = 32;
enum State : std::uint32_t { Running = 1, Stopping = 2, Stopped = 3, StopFailed = 4 };
enum Origin : std::uint32_t { BootPlugin = 1, OnDemand = 2 };
enum ShutdownFlags : std::uint32_t {
    GraceRequested = 1, ForceRequested = 2, ExitConfirmed = 4, ExitDeadlineExpired = 8
};
struct Entry {
    char name[NameCapacity] = {};
    std::uint64_t pid = 0;
    std::uint64_t serial = 0;
    std::uint64_t stop_started_tick = 0;
    State state = Stopped;
    std::uint32_t launches_this_hen = 0;
    Origin origin = OnDemand;
    std::uint32_t last_result = 0;
    std::uint32_t shutdown_flags = 0;
    bool occupied = false;
};
class Registry {
public:
    std::uint32_t count() const {
        std::uint32_t n=0;
        for(const auto& entry: entries_) if(entry.occupied) ++n;
        return n;
    }
    const Entry* at(std::uint32_t index) const {
        for (const auto& entry: entries_) {
            if (!entry.occupied) continue;
            if (!index--) return &entry;
        }
        return nullptr;
    }
    Entry* find(const char* name) {
        if(!validName(name)) return nullptr;
        for(auto& entry: entries_) {
            if(entry.occupied && std::strcmp(entry.name,name)==0) return &entry;
        }
        return nullptr;
    }
    static bool validName(const char* name) {
        if (!name) return false;
        bool any=false;
        for (unsigned i=0;i<NameCapacity;i++) {
            const char c=name[i];
            if(!c) return any;
            if (!((c>='a' && c<='z') || (c>='A' && c<='Z') ||
                  (c>='0' && c<='9') || c=='-' || c=='_')) return false;
            any=true;
        }
        return false; // A non-NUL-terminated name is rejected.
    }
    bool track(const char* name, std::uint64_t pid, std::uint64_t serial, Origin origin) {
        if (!validName(name) || !pid || (origin != BootPlugin && origin != OnDemand)) return false;
        // One live kernel process has exactly one managed name. Aliases
        // would otherwise allow stopping one plugin through another label.
        for (const auto& item:entries_)
            if (item.occupied && item.state != Stopped && item.pid == pid &&
                std::strcmp(item.name,name)) return false;
        Entry* target=find(name);
        if (target && (target->state!=Stopped))
            return target->pid==pid; // Idempotent; never replace an alive PID.
        if (!target) for(auto& item:entries_) if(!item.occupied){target=&item;break;}
        if (!target) return false;
        const std::uint32_t previous_launches = target->launches_this_hen;
        *target={};
        target->launches_this_hen = previous_launches + 1;
        std::memcpy(target->name,name,std::strlen(name)+1);
        target->pid=pid;
        target->serial=serial;
        target->origin=origin;
        target->state=Running;
        target->occupied=true;
        return true;
    }
    bool beginStop(const char* name, std::uint64_t* pid, std::uint64_t started_tick = 0) {
        Entry* e=find(name);
        if(!e || (e->state!=Running && e->state!=StopFailed)) return false;
        e->state=Stopping;
        e->stop_started_tick=started_tick;
        e->last_result=0;
        e->shutdown_flags=0;
        if(pid) *pid=e->pid;
        return true;
    }
    bool stopFailed(const char* name, std::uint64_t pid, std::uint32_t result) {
        Entry* e=find(name);
        if(!e || e->state!=Stopping || e->pid!=pid) return false;
        e->state=StopFailed;
        e->last_result=result;
        return true;
    }
    bool noteShutdown(const char* name, std::uint64_t pid, std::uint32_t flags) {
        Entry* e=find(name);
        if (!e || e->pid!=pid || (e->state!=Stopping && e->state!=StopFailed)) return false;
        e->shutdown_flags |= flags;
        return true;
    }
    bool exited(const char* name, std::uint64_t pid) {
        Entry* e=find(name);
        if(!e || e->pid!=pid || e->state==Stopped) return false;
        e->state=Stopped;
        e->shutdown_flags |= ExitConfirmed;
        e->stop_started_tick=0;
        e->last_result=0;
        return true;
    }
private:
    Entry entries_[RegistryCapacity] = {};
};
}
