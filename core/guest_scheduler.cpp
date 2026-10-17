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
GuestScheduler::Id GuestScheduler::create(std::uint64_t entry, std::uint64_t bottom, std::uint64_t top, std::uint64_t ret, std::uint64_t arg, std::uint64_t fs_base) {
    if (!entry || !ret || bottom>=top || top-bottom<16 || top<8 || next_==0) return 0;
    for (const auto& kv:threads_) if (bottom<kv.second.stack_top && kv.second.stack_bottom<top) return 0;
    // The return address occupies [rsp, rsp+8); at SysV function entry rsp % 16 == 8.
    const auto slot=((top-16)&~std::uint64_t(15))+8;
    if (slot<bottom || mem_.store(slot,&ret,8)!=MemoryError::ok) return 0;
    const Id id=next_++;
    auto& t=threads_[id]; t.cpu.rip=entry; t.cpu.gpr[4]=slot; t.cpu.gpr[7]=arg;
    t.cpu.rflags = 0x202;
    if (fs_base) {
        t.cpu.fs_base = fs_base;
    } else if (!threads_.empty() && threads_.count(1)) {
        t.cpu.fs_base = threads_.at(1).cpu.fs_base;
    }
    t.stack_bottom=bottom; t.stack_top=top;
    return id;
}
bool GuestScheduler::abort_created(Id id) {
    auto it = threads_.find(id);
    if (it == threads_.end()) return false;
    if (it->second.state != State::created) return false;
    threads_.erase(it);
    return true;
}
GuestScheduler::Id GuestScheduler::adopt(const CpuState& state, std::uint64_t bottom, std::uint64_t top) {
    const Id id = next_++;
    auto& t = threads_[id];
    t.cpu = state;
    t.stack_bottom = bottom;
    t.stack_top = top;
    t.state = State::ready;
    ready_.push_back(id);
    return id;
}
std::int32_t GuestScheduler::start(Id id) {
    auto* t=find(id); if (!t) return ORBIS_KERNEL_ERROR_ESRCH;
    if (t->state!=State::created) return ORBIS_KERNEL_ERROR_EINVAL;
    t->state=State::ready; ready_.push_back(id); return ORBIS_OK;
}
void GuestScheduler::ready(Id id, std::int32_t result) {
    auto* t=find(id); if (!t || t->state!=State::waiting) return;
    if (id == 1 || id == 1717) std::printf("[SCHED] ready TID %u res=0x%x\n", id, result);
    t->state=State::ready; t->wait=Wait::none; t->timed=false; t->result=result; ready_.push_back(id);
}
void GuestScheduler::block(Id id, Wait kind, Id object, std::uint64_t timeout, std::int32_t need) {
    auto& t=*find(id); t.state=State::waiting; t.wait=kind; t.object=object;
    t.result=ORBIS_OK; t.timed=timeout!=0;
    t.deadline=timeout>UINT64_MAX-now_ ? UINT64_MAX : now_+timeout;
    t.need = need;
}
std::int32_t GuestScheduler::resume(Id id) const { auto* t=find(id); return t?t->result:ORBIS_KERNEL_ERROR_ESRCH; }
GuestScheduler::State GuestScheduler::state(Id id) const { auto* t=find(id); return t?t->state:State::faulted; }
const CpuState& GuestScheduler::cpu(Id id) const { auto* t=find(id); if (!t) throw std::out_of_range("guest thread"); return t->cpu; }
CpuState& GuestScheduler::cpu(Id id) { auto* t=find(id); if (!t) throw std::out_of_range("guest thread"); return t->cpu; }
std::uint64_t GuestScheduler::value(Id id) const { auto* t=find(id); if (!t) throw std::out_of_range("guest thread"); return t->result_value; }
std::int32_t GuestScheduler::exit(Id id, std::uint64_t v) {
    auto* t=find(id); if (!t) return ORBIS_KERNEL_ERROR_ESRCH;
    if (t->state==State::exited || t->state==State::faulted) return ORBIS_KERNEL_ERROR_EINVAL;
    for (int iter = 0; iter < 4; ++iter) {
        bool had_destructors = false;
        for (const auto& key : tls_keys_) {
            auto val_it = t->tls_values.find(key);
            if (val_it != t->tls_values.end() && val_it->second != 0) {
                auto d_it = tls_destructors_.find(key);
                std::uint64_t val = val_it->second;
                val_it->second = 0;
                if (d_it != tls_destructors_.end() && d_it->second != 0) {
                    had_destructors = true;
                    if (tls_destructor_invoker_) {
                        tls_destructor_invoker_(id, d_it->second, val);
                    }
                }
            }
        }
        if (!had_destructors) break;
    }
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
    if (t->wait==Wait::cond || t->wait==Wait::reacquire) {
        if (t->wait == Wait::cond) {
            auto it = conds_.find(t->object);
            if (it != conds_.end()) {
                auto& w = it->second.waiters;
                w.erase(std::remove(w.begin(), w.end(), id), w.end());
            }
        }
        reacquire(id,t->mutex,ORBIS_KERNEL_ERROR_ECANCELED);
        return ORBIS_OK;
    }
    if (t->wait==Wait::mutex) {
        auto& w = mutexes_.at(t->object).waiters;
        w.erase(std::remove(w.begin(), w.end(), id), w.end());
    }
    if (t->wait==Wait::sema) {
        auto it = semas_.find(t->object);
        if (it != semas_.end()) {
            auto& w = it->second.waiters;
            w.erase(std::remove(w.begin(), w.end(), id), w.end());
        }
    }
    ready(id,ORBIS_KERNEL_ERROR_ECANCELED); return ORBIS_OK;
}
void GuestScheduler::advance(std::uint64_t us) {
    now_=us>UINT64_MAX-now_?UINT64_MAX:now_+us;
    for (auto& kv:threads_) {
        auto& t=kv.second;
        if (t.state!=State::waiting || !t.timed || now_<t.deadline) continue;
        if (t.wait==Wait::cond || t.wait==Wait::reacquire) {
            if (t.wait == Wait::cond) {
                auto it = conds_.find(t.object);
                if (it != conds_.end()) {
                    auto& w = it->second.waiters;
                    w.erase(std::remove(w.begin(), w.end(), kv.first), w.end());
                }
            }
            reacquire(kv.first,t.mutex,ORBIS_KERNEL_ERROR_ETIMEDOUT);
        }
        else if (t.wait==Wait::mutex) {
            auto& w = mutexes_.at(t.object).waiters;
            w.erase(std::remove(w.begin(), w.end(), kv.first), w.end());
            ready(kv.first, ORBIS_KERNEL_ERROR_ETIMEDOUT);
        }
        else if (t.wait==Wait::sema) {
            auto it = semas_.find(t.object);
            if (it != semas_.end()) {
                auto& w = it->second.waiters;
                w.erase(std::remove(w.begin(), w.end(), kv.first), w.end());
            }
            ready(kv.first, ORBIS_KERNEL_ERROR_ETIMEDOUT);
        }
        else ready(kv.first,t.wait==Wait::sleep?ORBIS_OK:ORBIS_KERNEL_ERROR_ETIMEDOUT);
    }
}
std::int32_t GuestScheduler::sleep(Id id, std::uint64_t us) {
    auto* t=find(id); if (!t || t->state!=State::ready) return ORBIS_KERNEL_ERROR_EINVAL;
    if (us) block(id,Wait::sleep,0,us);
    return ORBIS_OK;
}
GuestScheduler::Id GuestScheduler::mutex_create(bool recursive) { Id id=next_++; mutexes_[id].recursive=recursive; return id; }
std::int32_t GuestScheduler::mutex_delete(Id id) {
    auto it = mutexes_.find(id);
    if (it == mutexes_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
    for (Id wtid : it->second.waiters) {
        auto* wt = find(wtid);
        if (wt && wt->state == State::waiting && (wt->wait == Wait::mutex || wt->wait == Wait::reacquire) && wt->object == id) {
            ready(wtid, ORBIS_KERNEL_ERROR_ECANCELED);
        }
    }
    for (auto& pair : threads_) {
        auto& t = pair.second;
        if (t.state == State::waiting && t.wait == Wait::cond && t.mutex == id) {
            ready(pair.first, ORBIS_KERNEL_ERROR_ECANCELED);
        }
    }
    mutexes_.erase(it);
    return ORBIS_OK;
}
std::int32_t GuestScheduler::lock(Id tid, Id id, std::uint64_t timeout) {
    auto* t=find(tid); auto it=mutexes_.find(id);
    if (!t || t->state!=State::ready || it==mutexes_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
    auto& m=it->second;
    if (!m.owner) { m.owner=tid; m.depth=1; return ORBIS_OK; }
    if (m.owner==tid) { if (!m.recursive || m.depth==UINT_MAX) return ORBIS_KERNEL_ERROR_EDEADLK; ++m.depth; return ORBIS_OK; }
    block(tid,Wait::mutex,id,timeout); m.waiters.push_back(tid); return ORBIS_OK;
}
std::int32_t GuestScheduler::try_lock(Id tid, Id id) {
    auto* t=find(tid); auto it=mutexes_.find(id);
    if (!t || t->state!=State::ready || it==mutexes_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
    auto& m = it->second;
    if (m.owner) {
        if (m.owner == tid && m.recursive && m.depth < UINT_MAX) {
            ++m.depth;
            return ORBIS_OK;
        }
        return ORBIS_KERNEL_ERROR_EBUSY;
    }
    m.owner = tid;
    m.depth = 1;
    return ORBIS_OK;
}
unsigned GuestScheduler::mutex_depth(Id id) const {
    auto it = mutexes_.find(id);
    return it == mutexes_.end() ? 0 : it->second.depth;
}
void GuestScheduler::acquire_next(Id id) {
    auto& m=mutexes_.at(id);
    while (!m.waiters.empty()) {
        const auto tid=m.waiters.front(); m.waiters.pop_front();
        auto* t=find(tid);
        if (!t || t->state!=State::waiting || (t->wait!=Wait::mutex && t->wait!=Wait::reacquire) || t->object!=id) continue;
        unsigned depth = 1;
        if (t->wait == Wait::reacquire && t->saved_mutex_depth > 0) {
            depth = t->saved_mutex_depth;
        }
        m.owner=tid; m.depth=depth; ready(tid,t->result); return;
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
std::int32_t GuestScheduler::cond_delete(Id id) {
    auto it = conds_.find(id);
    if (it == conds_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
    auto waiters = std::move(it->second.waiters);
    conds_.erase(it);

    for (Id wtid : waiters) {
        auto* wt = find(wtid);
        if (wt && wt->state == State::waiting && wt->wait == Wait::cond && wt->object == id) {
            if (wt->mutex && mutexes_.count(wt->mutex)) {
                reacquire(wtid, wt->mutex, ORBIS_KERNEL_ERROR_ECANCELED);
            } else {
                ready(wtid, ORBIS_KERNEL_ERROR_ECANCELED);
            }
        }
    }
    return ORBIS_OK;
}
std::int32_t GuestScheduler::cond_wait(Id tid, Id cid, Id mid, std::uint64_t timeout) {
    auto* t=find(tid); auto c=conds_.find(cid); auto m=mutexes_.find(mid);
    if (!t || t->state!=State::ready || c==conds_.end() || m==mutexes_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
    if (m->second.owner != tid) {
        return ORBIS_KERNEL_ERROR_EPERM;
    }
    t->saved_mutex_depth = m->second.depth;
    block(tid,Wait::cond,cid,timeout); t->mutex=mid; c->second.waiters.push_back(tid);
    m->second.depth=0; m->second.owner=0; acquire_next(mid); return ORBIS_OK;
}
void GuestScheduler::reacquire(Id tid, Id mid, std::int32_t result) {
    auto* t=find(tid); if (!t || t->state!=State::waiting || (t->wait!=Wait::cond && t->wait!=Wait::reacquire)) return;
    auto& m=mutexes_.at(mid); t->result=result; t->timed=false;
    unsigned depth = (t->saved_mutex_depth > 0) ? t->saved_mutex_depth : 1;
    if (!m.owner) { m.owner=tid; m.depth=depth; ready(tid,result); }
    else if (t->wait!=Wait::reacquire) { t->wait=Wait::reacquire; t->object=mid; m.waiters.push_back(tid); }
}
std::int32_t GuestScheduler::cond_signal(Id cid, bool broadcast) {
    auto it=conds_.find(cid); if (it==conds_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
    while (!it->second.waiters.empty()) {
        Id tid=it->second.waiters.front(); it->second.waiters.pop_front();
        auto* t=find(tid);
        if (t && t->state==State::waiting && t->wait==Wait::cond && t->object==cid) {
            std::printf("[SCHED] cond_signal cid=%u waking TID %u\n", cid, tid);
            reacquire(tid,t->mutex,ORBIS_OK); if (!broadcast) break;
        }
    }
    return ORBIS_OK;
}
GuestScheduler::Id GuestScheduler::event_create(std::uint64_t initial) { Id id=next_++; events_[id].bits=initial; return id; }
std::int32_t GuestScheduler::event_delete(Id id) {
    auto it = events_.find(id);
    if (it == events_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
    for (Id wtid : it->second.waiters) {
        auto* wt = find(wtid);
        if (wt && wt->state == State::waiting && wt->wait == Wait::event && wt->object == id) {
            ready(wtid, ORBIS_KERNEL_ERROR_ECANCELED);
        }
    }
    events_.erase(it);
    return ORBIS_OK;
}
std::int32_t GuestScheduler::event_wait(Id tid, Id id, std::uint64_t mask, bool all, std::uint64_t timeout) {
    auto* t=find(tid); auto it=events_.find(id);
    if (!t || t->state!=State::ready || it==events_.end() || !mask) return ORBIS_KERNEL_ERROR_EINVAL;
    auto bits=it->second.bits&mask; if (all?bits==mask:bits!=0) return ORBIS_OK;
    block(tid,Wait::event,id,timeout); t->mask=mask; t->all=all; it->second.waiters.push_back(tid); return ORBIS_OK;
}
std::int32_t GuestScheduler::event_set(Id id, std::uint64_t bits) {
    auto it=events_.find(id); if (it==events_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
    it->second.bits|=bits;
    std::deque<Id> remaining_waiters;
    for (Id tid:it->second.waiters) {
        auto* t=find(tid);
        if (!t || t->state != State::waiting || t->wait != Wait::event || t->object != id) {
            // Obsoleto (acordado anteriormente, timeout ou destruído): descartar
            continue;
        }
        auto match=it->second.bits&t->mask;
        bool satisfied = t->all ? (match == t->mask) : (match != 0);
        if (satisfied) {
            ready(tid, ORBIS_OK);
        } else {
            remaining_waiters.push_back(tid);
        }
    }
    it->second.waiters = std::move(remaining_waiters);
    return ORBIS_OK;
}
std::int32_t GuestScheduler::event_clear(Id id, std::uint64_t bits) { auto it=events_.find(id); if (it==events_.end()) return ORBIS_KERNEL_ERROR_EINVAL; it->second.bits&=~bits; return ORBIS_OK; }
std::uint64_t GuestScheduler::event_bits(Id id) const {
    auto it=events_.find(id); return it==events_.end()?0:it->second.bits;
}
std::size_t GuestScheduler::event_waiters_count(Id id) const {
    auto it=events_.find(id); if (it==events_.end()) return 0;
    std::size_t count = 0;
    for (Id tid : it->second.waiters) {
        auto* t = find(tid);
        if (t && t->state == State::waiting && t->wait == Wait::event && t->object == id) {
            ++count;
        }
    }
    return count;
}

// Kernel Semaphores
GuestScheduler::Id GuestScheduler::sema_create(std::int32_t initial_count, std::int32_t max_count) {
    Id id = next_++;
    semas_[id].count = initial_count;
    semas_[id].max_count = max_count;
    return id;
}

std::int32_t GuestScheduler::sema_wait(Id tid, Id sid, std::int32_t need, std::uint64_t timeout) {
    auto* t = find(tid);
    auto it = semas_.find(sid);
    if (!t || t->state != State::ready || it == semas_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
    if (need <= 0 || need > it->second.max_count) return ORBIS_KERNEL_ERROR_EINVAL;

    auto& s = it->second;
    if (s.count >= need) {
        s.count -= need;
        return ORBIS_OK;
    }

    block(tid, Wait::sema, sid, timeout, need);
    s.waiters.push_back(tid);
    return ORBIS_OK;
}

std::int32_t GuestScheduler::sema_poll(Id sid, std::int32_t need) {
    auto it = semas_.find(sid);
    if (it == semas_.end()) return ORBIS_KERNEL_ERROR_ESRCH;
    if (need <= 0 || need > it->second.max_count) return ORBIS_KERNEL_ERROR_EINVAL;

    auto& s = it->second;
    if (s.count >= need) {
        s.count -= need;
        return ORBIS_OK;
    }
    return ORBIS_KERNEL_ERROR_EBUSY;
}

std::int32_t GuestScheduler::sema_signal(Id sid, std::int32_t count) {
    auto it = semas_.find(sid);
    if (it == semas_.end()) return ORBIS_KERNEL_ERROR_ESRCH;
    if (count <= 0) return ORBIS_KERNEL_ERROR_EINVAL;

    auto& s = it->second;
    if (count > s.max_count - s.count) return ORBIS_KERNEL_ERROR_EINVAL;

    s.count += count;

    // Wake waiters in FIFO order
    auto& waiters = s.waiters;
    for (auto wit = waiters.begin(); wit != waiters.end(); ) {
        Id wtid = *wit;
        auto* wt = find(wtid);
        if (wt && wt->state == State::waiting && wt->wait == Wait::sema && wt->object == sid && wt->need <= s.count) {
            s.count -= wt->need;
            wit = waiters.erase(wit);
            ready(wtid, ORBIS_OK);
        } else {
            ++wit;
        }
    }
    return ORBIS_OK;
}

std::int32_t GuestScheduler::sema_delete(Id sid) {
    auto it = semas_.find(sid);
    if (it == semas_.end()) return ORBIS_KERNEL_ERROR_ESRCH;

    // Wake all waiters with ECANCELED
    for (Id wtid : it->second.waiters) {
        auto* wt = find(wtid);
        if (wt && wt->state == State::waiting && wt->wait == Wait::sema && wt->object == sid) {
            ready(wtid, ORBIS_KERNEL_ERROR_ECANCELED);
        }
    }
    semas_.erase(it);
    return ORBIS_OK;
}

std::int32_t GuestScheduler::sema_count(Id sid) const {
    auto it = semas_.find(sid);
    if (it == semas_.end()) return -1;
    return it->second.count;
}

std::size_t GuestScheduler::sema_waiters(Id sid) const {
    auto it = semas_.find(sid);
    if (it == semas_.end()) return 0;
    return it->second.waiters.size();
}

// TLS (Thread-Local Storage)
GuestScheduler::Id GuestScheduler::tls_create(std::uint64_t destructor) {
    Id key = next_++;
    tls_keys_.insert(key);
    if (destructor) tls_destructors_[key] = destructor;
    return key;
}

std::int32_t GuestScheduler::tls_delete(Id key) {
    if (tls_keys_.find(key) == tls_keys_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
    tls_keys_.erase(key);
    tls_destructors_.erase(key);
    for (auto& kv : threads_) {
        kv.second.tls_values.erase(key);
    }
    return ORBIS_OK;
}

std::int32_t GuestScheduler::tls_set(Id tid, Id key, std::uint64_t value) {
    if (tls_keys_.find(key) == tls_keys_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
    auto* t = find(tid);
    if (!t) return ORBIS_KERNEL_ERROR_EINVAL;
    t->tls_values[key] = value;
    return ORBIS_OK;
}

std::uint64_t GuestScheduler::tls_get(Id tid, Id key) const {
    if (tls_keys_.find(key) == tls_keys_.end()) return 0;
    auto* t = find(tid);
    if (!t) return 0;
    auto it = t->tls_values.find(key);
    return it == t->tls_values.end() ? 0 : it->second;
}

bool GuestScheduler::tls_valid(Id key) const {
    return tls_keys_.find(key) != tls_keys_.end();
}

void GuestScheduler::set_tls_destructor_invoker(std::function<void(Id, std::uint64_t, std::uint64_t)> invoker) {
    tls_destructor_invoker_ = std::move(invoker);
}

void GuestScheduler::set_resume(Id id, std::function<void(CpuState&, std::int32_t)> hook) {
    auto* t=find(id); if (!t) throw std::logic_error("unknown thread for resume");
    t->on_resume=std::move(hook);
}
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
    while (out.steps<max_steps) {
        if (pause_checker_ && pause_checker_()) {
            out.stop = Stop::budget;
            return out;
        }
        if (ready_.empty()) {
            // Virtual time normally advances with retired instructions. When every
            // runnable thread sleeps, jump to the next finite wait deadline.
            std::uint64_t deadline=UINT64_MAX;
            bool timed=false;
            for (const auto& kv:threads_) {
                const auto& t=kv.second;
                if (t.state==State::waiting && t.timed) {
                    timed=true;
                    deadline=std::min(deadline,t.deadline);
                }
            }
            if (!timed) break;
            advance(deadline-now_);
            continue;
        }
        Id id=ready_.front(); ready_.pop_front(); auto& t=threads_.at(id);
        if (t.state!=State::ready) continue;
        if (t.on_resume) {
            auto hook=std::move(t.on_resume); t.on_resume=nullptr;
            hook(t.cpu,t.result);
        }
        interpreter_.state=t.cpu;
        const auto slice=std::min(quantum_,max_steps-out.steps);
        auto res=interpreter_.run(slice); t.cpu=interpreter_.state;
        out.steps+=res.steps; out.thread=id; out.cpu=res;
        if (res.stop==CpuStop::trap && trap_) {
            // Trap handler may only change scheduler state; it must not call run().
            if (!trap_(*this,id,t.cpu)) { t.state=State::faulted; out.stop=Stop::fault; return out; }
        } else if (res.stop == CpuStop::memory_fault && fault_handler_) {
            if (!fault_handler_(*this, id, t.cpu, res)) {
                t.state = State::faulted;
                out.stop = Stop::fault;
                return out;
            }
        } else if (res.stop!=CpuStop::step_limit) { t.state=State::faulted; out.stop=Stop::fault; return out; }
        if (t.state==State::ready) ready_.push_back(id);
        if (clock_mode_ == ClockMode::Steps) {
            if (us_per_step_ == 1.0) {
                advance(res.steps); // one virtual microsecond per retired instruction
            } else {
                sub_us_accumulator_ += static_cast<double>(res.steps) * us_per_step_;
                auto whole_us = static_cast<std::uint64_t>(sub_us_accumulator_);
                if (whole_us > 0) {
                    sub_us_accumulator_ -= static_cast<double>(whole_us);
                    advance(whole_us);
                }
            }
        }
    }
    if (out.steps==max_steps) return out;
    for (const auto& kv:threads_) if (kv.second.state==State::waiting) { out.stop=Stop::deadlock; return out; }
    out.stop=Stop::idle; return out;
}
} // namespace bb
