#include "guest_scheduler.h"
#include "check.h"
#include <cstdint>

int main() {
    using namespace bb;
    GuestMemory mem(1<<20);
    CHECK(mem.map(0x1000,0x1000,read|write)==MemoryError::ok);
    CHECK(mem.map(0x4000,0x1000,read|write)==MemoryError::ok);
    CHECK(mem.map(0x6000,0x1000,read|write)==MemoryError::ok);
    // inc rax; jmp -5: each context advances independently across quanta.
    const std::uint8_t code[]={0x48,0xff,0xc0,0xeb,0xfb};
    CHECK(mem.store(0x1000,code,sizeof(code))==MemoryError::ok);
    GuestScheduler s(mem,2);
    auto a=s.create(0x1000,0x4000,0x5000,0x2000);
    auto b=s.create(0x1000,0x6000,0x7000,0x2000);
    CHECK(a && b && a!=b);
    CHECK(s.create(0x1000,0x4000,0x5000,0x2000)==0);
    CHECK(s.start(a)==ORBIS_OK && s.start(b)==ORBIS_OK);
    auto result=s.run(20);
    CHECK(result.stop==GuestScheduler::Stop::budget && result.steps==20);
    CHECK(s.cpu(a).gpr[0]==5 && s.cpu(b).gpr[0]==5);
    CHECK(s.now()==20);
    auto mutex=s.mutex_create(), cond=s.cond_create(), event=s.event_create();
    CHECK(s.lock(a,mutex)==ORBIS_OK);
    CHECK(s.lock(b,mutex,5)==ORBIS_OK);
    CHECK(s.state(b)==GuestScheduler::State::waiting);
    s.advance(5);
    CHECK(s.resume(b)==ORBIS_KERNEL_ERROR_ETIMEDOUT);
    CHECK(s.unlock(a,mutex)==ORBIS_OK);
    CHECK(s.lock(b,mutex)==ORBIS_OK);
    CHECK(s.cond_wait(b,cond,mutex,3)==ORBIS_OK);
    CHECK(s.lock(a,mutex)==ORBIS_OK);
    CHECK(s.cond_signal(cond)==ORBIS_OK);
    CHECK(s.state(b)==GuestScheduler::State::waiting); // must reacquire
    CHECK(s.unlock(a,mutex)==ORBIS_OK);
    CHECK(s.state(b)==GuestScheduler::State::ready && s.resume(b)==ORBIS_OK);
    CHECK(s.unlock(b,mutex)==ORBIS_OK);
    CHECK(s.event_wait(a,event,3,true,4)==ORBIS_OK);
    CHECK(s.event_set(event,1)==ORBIS_OK && s.state(a)==GuestScheduler::State::waiting);
    CHECK(s.event_set(event,2)==ORBIS_OK && s.state(a)==GuestScheduler::State::ready);
    CHECK(s.join(a,b)==ORBIS_OK);
    CHECK(s.run(10).steps==10); // b cannot starve a; a is waiting
    CHECK(s.exit(b,123)==ORBIS_OK);
    CHECK(s.state(a)==GuestScheduler::State::ready && s.value(b)==123);
    CHECK(s.join(a,a)==ORBIS_KERNEL_ERROR_EDEADLK);
    CHECK(s.sleep(a,10)==ORBIS_OK);
    CHECK(s.run(10).stop==GuestScheduler::Stop::deadlock);
    s.advance(10);
    CHECK(s.resume(a)==ORBIS_OK);
    CHECK(s.event_wait(a,event,8,false)==ORBIS_OK);
    CHECK(s.cancel(a)==ORBIS_OK && s.resume(a)==ORBIS_KERNEL_ERROR_ECANCELED);
    CHECK(s.begin_initializer(a,0x42,{0x123,0x456})==ORBIS_OK);
    CHECK(s.initializer(a).function==0x123);
    CHECK(s.begin_initializer(a,0x42,{0x123,0})==ORBIS_KERNEL_ERROR_EDEADLK);
    CHECK(s.complete_initializer(a,0x42,true)==ORBIS_OK);
    s.register_atexit({0x234,0});
    CHECK(s.atexit_callbacks().size()==1);
    return 0;
}
