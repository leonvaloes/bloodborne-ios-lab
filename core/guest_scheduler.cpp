#include "guest_scheduler.h"
#include <algorithm>
#include <climits>
#include <limits>
#include <stdexcept>

namespace bb {
GuestScheduler::GuestScheduler(GuestMemory& memory, std::uint64_t quantum)
    : mem_(memory), interpreter_(memory), quantum_(quantum) {
    if (!quantum) throw std::invalid_argument("zero guest quantum");
}
void GuestScheduler::add_trap_range(std::uint64_t base, std::uint64_t size) { interpreter_.add_trap_range(base, size); }
GuestScheduler::Thread* GuestScheduler::find(Id id) { auto it=threads_.find(id); return it==threads_.end()?nullptr:&it->second; }
const GuestScheduler::Thread* GuestScheduler::find(Id id) const { auto it=threads_.find(id); return it==threads_.end()?nullptr:&it->second; }
GuestScheduler::Id GuestScheduler::create(std::uint64_t entry, std::uint64_t bottom, std::uint64_t top, std::uint64_t ret, std::uint64_t arg) {
    if (!entry || !ret || bottom>=top || top-bottom<16 || top<8 || next_==0) return 0;
    for (const auto& kv:threads_) if (bottom<kv.second.stack_top && kv.second.stack_bottom<top) return 0;
    const auto rsp=(top-8)&~std::uint64_t(15); // SysV entry: rsp % 16 == 8
    const auto slot=rsp-8;
    if (slot<bottom || mem_.store(slot,&ret,8)!=MemoryError::ok) return 0;
    const Id id=next_++;
    auto& t=threads_[id]; t.cpu.rip=entry; t.cpu.gpr[4]=slot; t.cpu.gpr[7]=arg;
    t.stack_bottom=bottom; t.stack_top=top;
    return id;
}
std::int32_t GuestScheduler::start(Id id) {
    auto* t=find(id); if (!t) return ORBIS_KERNEL_ERROR_ESRCH;
    if (t->state!=State::created) return ORBIS_KERNEL_ERROR_EINVAL;
    t->state=State::ready; ready_.push_back(id); return ORBIS_OK;
}
void GuestScheduler::ready(Id id, std::int32_t result) {
    auto* t=find(id); if (!t || t->state!=State::waiting) return;
    t->state=State::ready; t->wait=Wait::none; t->timed=false; t->result=result; ready_.push_back(id);
}
void GuestScheduler::block(Id id, Wait kind, Id object, std::uint64_t timeout) {
    auto& t=*find(id); t.state=State::waiting; t.wait=kind; t.object=object;
    t.result=ORBIS_OK; t.timed=timeout!=0;
    t.deadline=timeout>UINT64_MAX-now_ ? UINT64_MAX : now_+timeout;
}
std::int32_t GuestScheduler::resume(Id id) const { auto* t=find(id); return t?t->result:ORBIS_KERNEL_ERROR_ESRCH; }
GuestScheduler::State GuestScheduler::state(Id id) const { auto* t=find(id); return t?t->state:State::faulted; }
const CpuState& GuestScheduler::cpu(Id id) const { auto* t=find(id); if (!t) throw std::out_of_range("guest thread"); return t->cpu; }
std::uint64_t GuestScheduler::value(Id id) const { auto* t=find(id); if (!t) throw std::out_of_range("guest thread"); return t->result_value; }
std::int32_t GuestScheduler::exit(Id id, std::uint64_t v) {
    auto* t=find(id); if (!t) return ORBIS_KERNEL_ERROR_ESRCH;
    if (t->state==State::exited || t->state==State::faulted) return ORBIS_KERNEL_ERROR_EINVAL;
    t->state=State::exited; t->result_value=v;
    for (auto& kv:threads_) if (kv.second.state==State::waiting && kv.second.wait==Wait::join && kv.second.object==id) ready(kv.first,ORBIS_OK);
    return ORBIS_OK;
}
std::int32_t GuestScheduler::join(Id caller, Id target, std::uint64_t* out, std::uint64_t timeout) {
    auto* a=find(caller); auto* b=find(target);
    if (!a || !b) return ORBIS_KERNEL_ERROR_ESRCH;
    if (caller==target) return ORBIS_KERNEL_ERROR_EDEADLK;
    if (a->state!=State::ready) return ORBIS_KERNEL_ERROR_EINVAL;
    if (out) *out=0; // delayed join value is retrieved with value(target) after resume
    if (b->state==State::exited) { if (out) *out=b->result_value; return ORBIS_OK; }
    if (b->state==State::faulted) return ORBIS_KERNEL_ERROR_ECANCELED;
    block(caller,Wait::join,target,timeout); return ORBIS_OK;
}
std::int32_t GuestScheduler::cancel(Id id) {
    auto* t=find(id); if (!t || t->state!=State::waiting) return ORBIS_KERNEL_ERROR_EINVAL;
    if (t->wait==Wait::cond || t->wait==Wait::reacquire) { reacquire(id,t->mutex,ORBIS_KERNEL_ERROR_ECANCELED); return ORBIS_OK; }
    ready(id,ORBIS_KERNEL_ERROR_ECANCELED); return ORBIS_OK;
}
void GuestScheduler::advance(std::uint64_t us) {
    now_=us>UINT64_MAX-now_?UINT64_MAX:now_+us;
    for (auto& kv:threads_) {
        auto& t=kv.second;
        if (t.state!=State::waiting || !t.timed || now_<t.deadline) continue;
        if (t.wait==Wait::cond || t.wait==Wait::reacquire) reacquire(kv.first,t.mutex,ORBIS_KERNEL_ERROR_ETIMEDOUT);
        else ready(kv.first,t.wait==Wait::sleep?ORBIS_OK:ORBIS_KERNEL_ERROR_ETIMEDOUT);
    }
}
std::int32_t GuestScheduler::sleep(Id id, std::uint64_t us) {
    auto* t=find(id); if (!t || t->state!=State::ready) return ORBIS_KERNEL_ERROR_EINVAL;
    if (us) block(id,Wait::sleep,0,us);
    return ORBIS_OK;
}
GuestScheduler::Id GuestScheduler::mutex_create(bool recursive) { Id id=next_++; mutexes_[id].recursive=recursive; return id; }
std::int32_t GuestScheduler::lock(Id tid, Id id, std::uint64_t timeout) {
    auto* t=find(tid); auto it=mutexes_.find(id);
    if (!t || t->state!=State::ready || it==mutexes_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
    auto& m=it->second;
    if (!m.owner) { m.owner=tid; m.depth=1; return ORBIS_OK; }
    if (m.owner==tid) { if (!m.recursive || m.depth==UINT_MAX) return ORBIS_KERNEL_ERROR_EDEADLK; ++m.depth; return ORBIS_OK; }
    block(tid,Wait::mutex,id,timeout); m.waiters.push_back(tid); return ORBIS_OK;
}
void GuestScheduler::acquire_next(Id id) {
    auto& m=mutexes_.at(id);
    while (!m.waiters.empty()) {
        const auto tid=m.waiters.front(); m.waiters.pop_front();
        auto* t=find(tid);
        if (!t || t->state!=State::waiting || (t->wait!=Wait::mutex && t->wait!=Wait::reacquire) || t->object!=id) continue;
        m.owner=tid; m.depth=1; ready(tid,t->result); return;
    }
    m.owner=0; m.depth=0;
}
std::int32_t GuestScheduler::unlock(Id tid, Id id) {
    auto it=mutexes_.find(id); if (it==mutexes_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
    if (it->second.owner!=tid) return ORBIS_KERNEL_ERROR_EPERM;
    if (--it->second.depth==0) acquire_next(id);
    return ORBIS_OK;
}
GuestScheduler::Id GuestScheduler::cond_create() { Id id=next_++; conds_[id]; return id; }
std::int32_t GuestScheduler::cond_wait(Id tid, Id cid, Id mid, std::uint64_t timeout) {
    auto* t=find(tid); auto c=conds_.find(cid); auto m=mutexes_.find(mid);
    if (!t || t->state!=State::ready || c==conds_.end() || m==mutexes_.end() || m->second.owner!=tid || m->second.depth!=1) return ORBIS_KERNEL_ERROR_EINVAL;
    block(tid,Wait::cond,cid,timeout); t->mutex=mid; c->second.waiters.push_back(tid);
    m->second.depth=0; acquire_next(mid); return ORBIS_OK;
}
void GuestScheduler::reacquire(Id tid, Id mid, std::int32_t result) {
    auto* t=find(tid); if (!t || t->state!=State::waiting || (t->wait!=Wait::cond && t->wait!=Wait::reacquire)) return;
    auto& m=mutexes_.at(mid); t->result=result; t->timed=false;
    if (!m.owner) { m.owner=tid; m.depth=1; ready(tid,result); }
    else if (t->wait!=Wait::reacquire) { t->wait=Wait::reacquire; t->object=mid; m.waiters.push_back(tid); }
}
std::int32_t GuestScheduler::cond_signal(Id cid, bool broadcast) {
    auto it=conds_.find(cid); if (it==conds_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
    while (!it->second.waiters.empty()) {
        Id tid=it->second.waiters.front(); it->second.waiters.pop_front();
        auto* t=find(tid);
        if (t && t->state==State::waiting && t->wait==Wait::cond && t->object==cid) {
            reacquire(tid,t->mutex,ORBIS_OK); if (!broadcast) break;
        }
    }
    return ORBIS_OK;
}
GuestScheduler::Id GuestScheduler::event_create(std::uint64_t initial) { Id id=next_++; events_[id].bits=initial; return id; }
std::int32_t GuestScheduler::event_wait(Id tid, Id id, std::uint64_t mask, bool all, std::uint64_t timeout) {
    auto* t=find(tid); auto it=events_.find(id);
    if (!t || t->state!=State::ready || it==events_.end() || !mask) return ORBIS_KERNEL_ERROR_EINVAL;
    auto bits=it->second.bits&mask; if (all?bits==mask:bits!=0) return ORBIS_OK;
    block(tid,Wait::event,id,timeout); t->mask=mask; t->all=all; it->second.waiters.push_back(tid); return ORBIS_OK;
}
std::int32_t GuestScheduler::event_set(Id id, std::uint64_t bits) {
    auto it=events_.find(id); if (it==events_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
    it->second.bits|=bits;
    for (Id tid:it->second.waiters) {
        auto* t=find(tid);
        if (t && t->state==State::waiting && t->wait==Wait::event && t->object==id) {
            auto match=it->second.bits&t->mask;
            if (t->all?match==t->mask:match!=0) ready(tid,ORBIS_OK);
        }
    }
    return ORBIS_OK;
}
std::int32_t GuestScheduler::event_clear(Id id, std::uint64_t bits) { auto it=events_.find(id); if (it==events_.end()) return ORBIS_KERNEL_ERROR_EINVAL; it->second.bits&=~bits; return ORBIS_OK; }
std::int32_t GuestScheduler::begin_initializer(Id tid, std::uint64_t key, Callback cb) {
    auto* t=find(tid); if (!t || t->state!=State::ready || !key || !cb.function) return ORBIS_KERNEL_ERROR_EINVAL;
    auto& init=inits_[key]; if (init.complete) return ORBIS_OK;
    if (init.owner==tid) return ORBIS_KERNEL_ERROR_EDEADLK;
    if (init.owner) { block(tid,Wait::init,0,0); init.waiters.push_back(tid); return ORBIS_OK; }
    init.owner=tid; t->callback=cb; return ORBIS_OK;
}
std::int32_t GuestScheduler::complete_initializer(Id tid, std::uint64_t key, bool success) {
    auto it=inits_.find(key); if (it==inits_.end() || it->second.owner!=tid) return ORBIS_KERNEL_ERROR_EPERM;
    it->second.owner=0; it->second.complete=success;
    for (Id w:it->second.waiters) ready(w,success?ORBIS_OK:ORBIS_KERNEL_ERROR_ECANCELED);
    it->second.waiters.clear(); find(tid)->callback={}; return ORBIS_OK;
}
GuestScheduler::Callback GuestScheduler::initializer(Id tid) const { auto* t=find(tid); return t?t->callback:Callback{}; }
void GuestScheduler::register_atexit(Callback cb) { if (cb.function) atexit_.push_back(cb); }
GuestScheduler::Result GuestScheduler::run(std::uint64_t max_steps) {
    if (running_) throw std::logic_error("reentrant guest scheduler run");
    struct RunGuard { bool& flag; ~RunGuard() { flag=false; } } guard{running_};
    running_=true;
    Result out{Stop::budget,0,0,{}};
    while (out.steps<max_steps && !ready_.empty()) {
        Id id=ready_.front(); ready_.pop_front(); auto& t=threads_.at(id);
        if (t.state!=State::ready) continue;
        interpreter_.state=t.cpu;
        const auto slice=std::min(quantum_,max_steps-out.steps);
        auto res=interpreter_.run(slice); t.cpu=interpreter_.state;
        out.steps+=res.steps; out.thread=id; out.cpu=res;
        if (res.stop==CpuStop::trap && trap_) {
            // Trap handler may only change scheduler state; it must not call run().
            if (!trap_(*this,id,t.cpu)) { t.state=State::faulted; out.stop=Stop::fault; return out; }
        } else if (res.stop!=CpuStop::step_limit) { t.state=State::faulted; out.stop=Stop::fault; return out; }
        if (t.state==State::ready) ready_.push_back(id);
        advance(res.steps); // one virtual microsecond per retired instruction
    }
    if (out.steps==max_steps) return out;
    for (const auto& kv:threads_) if (kv.second.state==State::waiting) { out.stop=Stop::deadlock; return out; }
    out.stop=Stop::idle; return out;
}
} // namespace bb
