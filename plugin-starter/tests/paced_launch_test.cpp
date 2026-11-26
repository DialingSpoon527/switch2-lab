#include <cassert>
#include <cstdint>
#include <iostream>
#include "paced_loader_step.hpp"

struct Clock {
    std::uint64_t now=0;
    unsigned sleeps=0;
    void sleep(std::uint64_t ns){now+=ns;++sleeps;}
};
struct Gate {
    Clock& clock;
    unsigned begins=0, polls=0;
    std::uint64_t ready_at=0;
    std::uint32_t begin_error=0,poll_error=0;
    bool never=false;
    std::uint32_t beginStep(){++begins;return begin_error;}
    std::uint32_t pollStep(void*,bool& done){++polls;done=!never&&clock.now>=ready_at;return poll_error;}
};
int main(){
    const auto run=[](Gate& gate){return ounce::pacedLoaderStep(gate,nullptr,[&]{return gate.clock.now;},[&](std::uint64_t ns){gate.clock.sleep(ns);},99);};
    {Clock c;Gate g{c};assert(run(g)==0&&g.begins==1&&g.polls==1&&c.sleeps==0);}
    {Clock c;Gate g{c};g.ready_at=800000000ull;assert(run(g)==0&&g.polls>30&&c.sleeps>0);}
    {Clock c;Gate g{c};g.begin_error=7;assert(run(g)==7&&g.polls==0&&c.sleeps==0);}
    {Clock c;Gate g{c};g.poll_error=8;assert(run(g)==8&&g.begins==1&&g.polls==1&&c.sleeps==0);}
    {Clock c;Gate g{c};g.never=true;assert(run(g)==99&&c.now==1500000000ull&&g.begins==1);}
    for(unsigned state=0;state<8;state++)for(unsigned bits=0;bits<64;bits++){
        const bool init=bits&1,held=bits&2,restoring=bits&4,stepping=bits&8,capturing=bits&16,healthy=bits&32;
        const bool wait_game=state==0,wait_exit=state==1;
        const bool want=init&&!held&&!restoring&&!stepping&&!capturing&&healthy&&(wait_game||wait_exit);
        assert(ounce::canBorrowPluginLoader(init,held,restoring,stepping,capturing,wait_game,wait_exit,healthy)==want);
    }
    assert(ounce::canBorrowPluginLoader(true,false,false,false,false,false,true,true));
    std::cout<<"PASS: paced late completion, begin/poll errors, bounded deadline and 512 live-host lease combinations\n";
}
