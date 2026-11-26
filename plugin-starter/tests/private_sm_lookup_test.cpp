#include <cassert>
#include <cstring>
#include <iostream>
#include "private_sm_lookup.hpp"
#include "cmif_server.hpp"
#include "inline_payload.hpp"

namespace fixture {
alignas(16) thread_local u8 message[0x100]={};
unsigned registrations=0,lookups=0,queries=0,closes=0;
Result lookup_error=0;
bool return_invalid=false;
void reply(Result result, Handle moved=INVALID_HANDLE, bool pointer_size=false, bool move_present=false){
    std::memset(message,0,sizeof(message));
    auto response=hipcMakeRequestInline(message,.num_data_words=pointer_size?9u:8u,.num_move_handles=moved!=INVALID_HANDLE||move_present?1u:0u);
    if(moved!=INVALID_HANDLE||move_present)response.move_handles[0]=moved;
    auto* header=static_cast<CmifOutHeader*>(cmifGetAlignedDataStart(response.data_words,message));
    header->magic=CMIF_OUT_HEADER_MAGIC;header->result=result;
}
void reset(){registrations=lookups=queries=closes=0;lookup_error=0;return_invalid=false;}
}
extern "C" {
void* armGetTls(void){return fixture::message;}
Result svcCloseHandle(Handle){++fixture::closes;return 0;}
Result svcSendSyncRequest(Handle handle){
    using namespace fixture;assert(handle==77); // Existing delegated proxy, never a new HEN-owned session.
    const auto request=ounce::parseRequest(message);assert(request.valid);
    assert(!request.hipc.meta.num_copy_handles&&!request.hipc.meta.num_move_handles);
    if(ounce::isQueryPointerBufferSize(request)){++queries;reply(0,INVALID_HANDLE,true);return 0;}
    assert(request.hipc.meta.type==CmifCommandType_Request);
    size_t length=0;assert(ounce::inlinePayloadSize(request,&length)&&length==8);
    assert(request.command_id==1&&registrations==0&&!request.hipc.meta.send_pid);
    assert(!std::memcmp(request.data,"mig:usr",8));++lookups;
    reply(lookup_error,lookup_error||return_invalid?INVALID_HANDLE:777,false,!lookup_error);return 0;
}
}
int main(){
    Handle output=0;u32 step=0;
    fixture::reset();assert(pluginmanager::lookupMigrationOnDelegatedSm(77,&output,&step)==0&&output==777&&!step);
    assert(fixture::queries==1&&!fixture::registrations&&fixture::lookups==1&&!fixture::closes);
    fixture::reset();fixture::lookup_error=0x1015;assert(pluginmanager::lookupMigrationOnDelegatedSm(77,&output,&step)==0x1015&&output==INVALID_HANDLE&&step==pluginmanager::LookupService);assert(!fixture::closes&&!fixture::registrations);
    fixture::reset();fixture::return_invalid=true;assert(pluginmanager::lookupMigrationOnDelegatedSm(77,&output,&step)!=0&&step==pluginmanager::LookupService);
    assert(pluginmanager::lookupMigrationOnDelegatedSm(77,nullptr,&step)!=0);
    std::cout<<"PASS: actual SDK CMIF delegated lookup, no registration/identity rebinding, moved handle, errors and borrowed ownership\n";
}
