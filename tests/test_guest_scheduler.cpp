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
    for (auto id : {a,b}) {
        const auto rsp=s.cpu(id).gpr[4];
        CHECK(rsp%16==8);
        CHECK(rsp>= (id==a?0x4000u:0x6000u) && rsp+8<= (id==a?0x5000u:0x7000u));
        std::uint64_t return_address=0;
        CHECK(mem.load(rsp,&return_address,8)==MemoryError::ok);
        CHECK(return_address==0x2000);
    }
    CHECK(s.create(0x1000,0x4000,0x5000,0x2000)==0);
    CHECK(mem.map(0xa000,0x1000,read|write)==MemoryError::ok);
    auto unaligned=s.create(0x1000,0xa000,0xa011,0x2345);
    CHECK(unaligned && s.cpu(unaligned).gpr[4]==0xa008);
    std::uint64_t unaligned_return=0;
    CHECK(mem.load(0xa008,&unaligned_return,8)==MemoryError::ok && unaligned_return==0x2345);
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
    CHECK(s.run(10).stop==GuestScheduler::Stop::budget);
    CHECK(s.now()>=30);
    CHECK(s.resume(a)==ORBIS_OK);
    CHECK(s.event_wait(a,event,8,false)==ORBIS_OK);
    CHECK(s.cancel(a)==ORBIS_OK && s.resume(a)==ORBIS_KERNEL_ERROR_ECANCELED);
    CHECK(s.begin_initializer(a,0x42,{0x123,0x456})==ORBIS_OK);
    CHECK(s.initializer(a).function==0x123);
    CHECK(s.begin_initializer(a,0x42,{0x123,0})==ORBIS_KERNEL_ERROR_EDEADLK);
    CHECK(s.complete_initializer(a,0x42,true)==ORBIS_OK);
    s.register_atexit({0x234,0});
    CHECK(s.atexit_callbacks().size()==1);

    // No runnable thread: a finite wait must expire without a false deadlock.
    GuestScheduler timers(mem,2);
    auto sleeper=timers.create(0x1000,0x8000,0x9000,0x2000);
    CHECK(sleeper==0); // unmapped stack is rejected
    CHECK(mem.map(0x8000,0x1000,read|write)==MemoryError::ok);
    sleeper=timers.create(0x1000,0x8000,0x9000,0x2000);
    CHECK(sleeper && timers.start(sleeper)==ORBIS_OK);
    CHECK(timers.sleep(sleeper,7)==ORBIS_OK);
    CHECK(timers.run(2).stop==GuestScheduler::Stop::budget);
    CHECK(timers.now()>=7 && timers.resume(sleeper)==ORBIS_OK);
    CHECK(timers.sleep(sleeper,0)==ORBIS_OK);
    auto pending=timers.event_create();
    CHECK(timers.event_wait(sleeper,pending,1,false)==ORBIS_OK);
    CHECK(timers.run(2).stop==GuestScheduler::Stop::deadlock);
    CHECK(timers.cancel(sleeper)==ORBIS_OK);

    // Cancelled/timed-out mutex waiters must not remain in the FIFO queue.
    GuestScheduler locks(mem,2);
    auto owner=locks.create(0x1000,0x4000,0x5000,0x2000);
    auto first=locks.create(0x1000,0x6000,0x7000,0x2000);
    auto second=locks.create(0x1000,0x8000,0x9000,0x2000);
    CHECK(owner && first && second);
    CHECK(locks.start(owner)==ORBIS_OK && locks.start(first)==ORBIS_OK && locks.start(second)==ORBIS_OK);
    auto m=locks.mutex_create();
    CHECK(locks.lock(owner,m)==ORBIS_OK);
    CHECK(locks.lock(first,m,3)==ORBIS_OK);
    CHECK(locks.lock(second,m)==ORBIS_OK);
    CHECK(locks.run(2).stop==GuestScheduler::Stop::budget);
    // O prazo é de três microssegundos virtuais; dois passos ainda não o esgotam.
    locks.advance(1);
    CHECK(locks.resume(first)==ORBIS_KERNEL_ERROR_ETIMEDOUT);
    CHECK(locks.unlock(owner,m)==ORBIS_OK);
    CHECK(locks.state(second)==GuestScheduler::State::ready && locks.unlock(second,m)==ORBIS_OK);
    CHECK(locks.lock(owner,m)==ORBIS_OK);
    CHECK(locks.lock(first,m)==ORBIS_OK);
    CHECK(locks.cancel(first)==ORBIS_OK);
    CHECK(locks.unlock(owner,m)==ORBIS_OK);
    CHECK(locks.try_lock(second,m)==ORBIS_OK);
    CHECK(locks.unlock(second,m)==ORBIS_OK);

    // ========================================================================
    // T069: Preservar profundidade de mutex recursivo em cond_wait
    // Rotas: (1) Sinalização, (2) Timeout, (3) Cancelamento
    // ========================================================================
    {
        GuestScheduler rec_sched(mem, 2);
        auto t_waiter = rec_sched.create(0x1000, 0x4000, 0x5000, 0x2000);
        auto t_signaler = rec_sched.create(0x1000, 0x6000, 0x7000, 0x2000);
        CHECK(t_waiter && t_signaler);
        CHECK(rec_sched.start(t_waiter) == ORBIS_OK && rec_sched.start(t_signaler) == ORBIS_OK);

        auto rec_m = rec_sched.mutex_create(true); // recursive = true
        auto rec_c = rec_sched.cond_create();

        // Rota 1: Sinalização (cond_signal) com profundidade = 3
        CHECK(rec_sched.lock(t_waiter, rec_m) == ORBIS_OK);
        CHECK(rec_sched.lock(t_waiter, rec_m) == ORBIS_OK);
        CHECK(rec_sched.lock(t_waiter, rec_m) == ORBIS_OK);
        CHECK(rec_sched.mutex_depth(rec_m) == 3);

        CHECK(rec_sched.cond_wait(t_waiter, rec_c, rec_m) == ORBIS_OK);
        CHECK(rec_sched.mutex_depth(rec_m) == 0);

        CHECK(rec_sched.lock(t_signaler, rec_m) == ORBIS_OK);
        CHECK(rec_sched.cond_signal(rec_c) == ORBIS_OK);
        CHECK(rec_sched.state(t_waiter) == GuestScheduler::State::waiting);
        CHECK(rec_sched.unlock(t_signaler, rec_m) == ORBIS_OK);

        CHECK(rec_sched.state(t_waiter) == GuestScheduler::State::ready);
        CHECK(rec_sched.resume(t_waiter) == ORBIS_OK);
        CHECK(rec_sched.mutex_depth(rec_m) == 3);

        CHECK(rec_sched.unlock(t_waiter, rec_m) == ORBIS_OK);
        CHECK(rec_sched.mutex_depth(rec_m) == 2);
        CHECK(rec_sched.unlock(t_waiter, rec_m) == ORBIS_OK);
        CHECK(rec_sched.mutex_depth(rec_m) == 1);
        CHECK(rec_sched.unlock(t_waiter, rec_m) == ORBIS_OK);
        CHECK(rec_sched.mutex_depth(rec_m) == 0);
        CHECK(rec_sched.unlock(t_waiter, rec_m) == ORBIS_KERNEL_ERROR_EPERM);

        // Rota 2: Timeout com profundidade = 2
        CHECK(rec_sched.lock(t_waiter, rec_m) == ORBIS_OK);
        CHECK(rec_sched.lock(t_waiter, rec_m) == ORBIS_OK);
        CHECK(rec_sched.mutex_depth(rec_m) == 2);

        CHECK(rec_sched.cond_wait(t_waiter, rec_c, rec_m, 5) == ORBIS_OK);
        CHECK(rec_sched.mutex_depth(rec_m) == 0);

        rec_sched.advance(5);
        CHECK(rec_sched.state(t_waiter) == GuestScheduler::State::ready);
        CHECK(rec_sched.resume(t_waiter) == ORBIS_KERNEL_ERROR_ETIMEDOUT);
        CHECK(rec_sched.mutex_depth(rec_m) == 2);

        // cond_signal posterior não deve afetar waiter que sofreu timeout
        CHECK(rec_sched.cond_signal(rec_c) == ORBIS_OK);

        CHECK(rec_sched.unlock(t_waiter, rec_m) == ORBIS_OK);
        CHECK(rec_sched.mutex_depth(rec_m) == 1);
        CHECK(rec_sched.unlock(t_waiter, rec_m) == ORBIS_OK);
        CHECK(rec_sched.mutex_depth(rec_m) == 0);

        // Rota 3: Cancelamento com profundidade = 4
        CHECK(rec_sched.lock(t_waiter, rec_m) == ORBIS_OK);
        CHECK(rec_sched.lock(t_waiter, rec_m) == ORBIS_OK);
        CHECK(rec_sched.lock(t_waiter, rec_m) == ORBIS_OK);
        CHECK(rec_sched.lock(t_waiter, rec_m) == ORBIS_OK);
        CHECK(rec_sched.mutex_depth(rec_m) == 4);

        CHECK(rec_sched.cond_wait(t_waiter, rec_c, rec_m) == ORBIS_OK);
        CHECK(rec_sched.mutex_depth(rec_m) == 0);

        CHECK(rec_sched.cancel(t_waiter) == ORBIS_OK);
        CHECK(rec_sched.state(t_waiter) == GuestScheduler::State::ready);
        CHECK(rec_sched.resume(t_waiter) == ORBIS_KERNEL_ERROR_ECANCELED);
        CHECK(rec_sched.mutex_depth(rec_m) == 4);

        // cond_signal posterior não deve afetar waiter que foi cancelado
        CHECK(rec_sched.cond_signal(rec_c) == ORBIS_OK);

        CHECK(rec_sched.unlock(t_waiter, rec_m) == ORBIS_OK);
        CHECK(rec_sched.unlock(t_waiter, rec_m) == ORBIS_OK);
        CHECK(rec_sched.unlock(t_waiter, rec_m) == ORBIS_OK);
        CHECK(rec_sched.unlock(t_waiter, rec_m) == ORBIS_OK);
        CHECK(rec_sched.mutex_depth(rec_m) == 0);
    }

    // ========================================================================
    // T068: Validação de chaves pthread TLS e execução de destrutores
    // ========================================================================
    {
        GuestScheduler tls_sched(mem, 2);
        auto t_worker = tls_sched.create(0x1000, 0x4000, 0x5000, 0x2000);
        CHECK(t_worker && tls_sched.start(t_worker) == ORBIS_OK);

        std::uint64_t destructor_called_with_val = 0;
        GuestScheduler::Id destructor_called_for_thread = 0;
        tls_sched.set_tls_destructor_invoker([&](GuestScheduler::Id tid, std::uint64_t dtor, std::uint64_t val) {
            if (dtor == 0xDEADBEEF) {
                destructor_called_for_thread = tid;
                destructor_called_with_val = val;
            }
        });

        // 1. Criação e validação de chaves
        auto key1 = tls_sched.tls_create(0xDEADBEEF);
        auto key2 = tls_sched.tls_create(0);
        CHECK(tls_sched.tls_valid(key1));
        CHECK(tls_sched.tls_valid(key2));
        CHECK(!tls_sched.tls_valid(99999));

        // 2. Erros com chaves inexistentes
        CHECK(tls_sched.tls_set(t_worker, 99999, 123) == ORBIS_KERNEL_ERROR_EINVAL);
        CHECK(tls_sched.tls_get(t_worker, 99999) == 0);
        CHECK(tls_sched.tls_delete(99999) == ORBIS_KERNEL_ERROR_EINVAL);

        // 3. Configuração de valores
        CHECK(tls_sched.tls_set(t_worker, key1, 0xCAFEBABE) == ORBIS_OK);
        CHECK(tls_sched.tls_set(t_worker, key2, 0x12345) == ORBIS_OK);
        CHECK(tls_sched.tls_get(t_worker, key1) == 0xCAFEBABE);
        CHECK(tls_sched.tls_get(t_worker, key2) == 0x12345);

        // 4. Exclusão de chave
        CHECK(tls_sched.tls_delete(key2) == ORBIS_OK);
        CHECK(!tls_sched.tls_valid(key2));
        CHECK(tls_sched.tls_get(t_worker, key2) == 0);
        CHECK(tls_sched.tls_delete(key2) == ORBIS_KERNEL_ERROR_EINVAL);

        // 5. Saída da thread e execução de destrutor para valor não nulo
        CHECK(tls_sched.exit(t_worker, 0) == ORBIS_OK);
        CHECK(destructor_called_for_thread == t_worker);
        CHECK(destructor_called_with_val == 0xCAFEBABE);

        // Após exit, valor TLS deve estar zerado
        CHECK(tls_sched.tls_get(t_worker, key1) == 0);
    }

    // ========================================================================
    // T074: mutex_delete, cond_delete e event_delete acordando waiters com ECANCELED
    // ========================================================================
    {
        GuestScheduler sched(mem, 2);
        auto t_owner = sched.create(0x1000, 0x4000, 0x5000, 0x2000);
        auto t_waiter1 = sched.create(0x1000, 0x6000, 0x7000, 0x2000);
        auto t_waiter2 = sched.create(0x1000, 0x8000, 0x9000, 0x2000);
        CHECK(t_owner && t_waiter1 && t_waiter2);
        CHECK(sched.start(t_owner) == ORBIS_OK && sched.start(t_waiter1) == ORBIS_OK && sched.start(t_waiter2) == ORBIS_OK);

        // 1. mutex_delete com waiters
        auto m_del = sched.mutex_create();
        CHECK(sched.lock(t_owner, m_del) == ORBIS_OK);
        CHECK(sched.lock(t_waiter1, m_del) == ORBIS_OK);
        CHECK(sched.lock(t_waiter2, m_del) == ORBIS_OK);
        CHECK(sched.state(t_waiter1) == GuestScheduler::State::waiting);
        CHECK(sched.state(t_waiter2) == GuestScheduler::State::waiting);

        CHECK(sched.mutex_delete(m_del) == ORBIS_OK);
        CHECK(sched.state(t_waiter1) == GuestScheduler::State::ready);
        CHECK(sched.state(t_waiter2) == GuestScheduler::State::ready);
        CHECK(sched.resume(t_waiter1) == ORBIS_KERNEL_ERROR_ECANCELED);
        CHECK(sched.resume(t_waiter2) == ORBIS_KERNEL_ERROR_ECANCELED);

        // Mutex já excluído não existe mais
        CHECK(sched.mutex_delete(m_del) == ORBIS_KERNEL_ERROR_EINVAL);
        CHECK(sched.mutex_delete(99999) == ORBIS_KERNEL_ERROR_EINVAL);

        // 2. cond_delete com waiters
        auto m_for_cond = sched.mutex_create();
        auto c_del = sched.cond_create();
        CHECK(sched.lock(t_waiter1, m_for_cond) == ORBIS_OK);
        CHECK(sched.cond_wait(t_waiter1, c_del, m_for_cond) == ORBIS_OK);
        CHECK(sched.state(t_waiter1) == GuestScheduler::State::waiting);

        CHECK(sched.cond_delete(c_del) == ORBIS_OK);
        CHECK(sched.state(t_waiter1) == GuestScheduler::State::ready);
        CHECK(sched.resume(t_waiter1) == ORBIS_KERNEL_ERROR_ECANCELED);
        CHECK(sched.cond_delete(c_del) == ORBIS_KERNEL_ERROR_EINVAL);
        CHECK(sched.cond_delete(99999) == ORBIS_KERNEL_ERROR_EINVAL);

        // 3. event_delete com waiters
        auto e_del = sched.event_create();
        CHECK(sched.event_wait(t_waiter2, e_del, 1, false) == ORBIS_OK);
        CHECK(sched.state(t_waiter2) == GuestScheduler::State::waiting);

        CHECK(sched.event_delete(e_del) == ORBIS_OK);
        CHECK(sched.state(t_waiter2) == GuestScheduler::State::ready);
        CHECK(sched.resume(t_waiter2) == ORBIS_KERNEL_ERROR_ECANCELED);
        CHECK(sched.event_delete(e_del) == ORBIS_KERNEL_ERROR_EINVAL);
        CHECK(sched.event_delete(99999) == ORBIS_KERNEL_ERROR_EINVAL);
    }

    // T064: Validação de relógio virtual desacoplado dos passos de instrução (pacing 30 FPS e cargas variáveis)
    {
        GuestMemory mem(1 << 20);
        CHECK(mem.map(0x1000, 0x1000, read | write) == MemoryError::ok);
        CHECK(mem.map(0x4000, 0x1000, read | write) == MemoryError::ok);
        CHECK(mem.map(0x6000, 0x1000, read | write) == MemoryError::ok);
        const std::uint8_t code[] = {0x48, 0xff, 0xc0, 0xeb, 0xfb}; // inc rax; jmp -5
        CHECK(mem.store(0x1000, code, sizeof(code)) == MemoryError::ok);

        GuestScheduler s(mem, 16);
        CHECK(s.clock_mode() == GuestScheduler::ClockMode::Steps);
        s.set_clock_mode(GuestScheduler::ClockMode::Decoupled);
        CHECK(s.clock_mode() == GuestScheduler::ClockMode::Decoupled);

        auto worker = s.create(0x1000, 0x4000, 0x5000, 0x2000);
        auto waiter = s.create(0x1000, 0x6000, 0x7000, 0x2000);
        CHECK(s.start(worker) == ORBIS_OK);
        CHECK(s.start(waiter) == ORBIS_OK);

        // Submete waiter para sleep de 33.333 us (1 frame a 30 FPS)
        CHECK(s.sleep(waiter, 33333) == ORBIS_OK);
        CHECK(s.state(waiter) == GuestScheduler::State::waiting);

        // Executa 100 passos de worker sob carga leve
        auto res1 = s.run(100);
        CHECK(res1.steps == 100);
        // Em modo desacoplado, passos de CPU NÃO devem avançar now_
        CHECK(s.now() == 0);
        CHECK(s.state(waiter) == GuestScheduler::State::waiting);

        // Executa 10.000 passos de worker sob carga pesada
        auto res2 = s.run(10000);
        CHECK(res2.steps == 10000);
        CHECK(s.now() == 0);
        CHECK(s.state(waiter) == GuestScheduler::State::waiting);

        // Pacing avança 16.666 us (meio quadro de 30 FPS)
        s.advance(16666);
        CHECK(s.now() == 16666);
        CHECK(s.state(waiter) == GuestScheduler::State::waiting);

        // Worker roda mais 5.000 passos
        auto res3 = s.run(5000);
        CHECK(res3.steps == 5000);
        CHECK(s.now() == 16666);
        CHECK(s.state(waiter) == GuestScheduler::State::waiting);

        // Pacing completa o quadro de 30 FPS (atingindo 33.333 us)
        s.advance(16667);
        CHECK(s.now() == 33333);
        // O timeout de sleep deve ter disparado deterministamente pelo relógio e não pelos passos
        CHECK(s.state(waiter) == GuestScheduler::State::ready);
        CHECK(s.resume(waiter) == ORBIS_OK);
    }

    // ========================================================================
    // T174: Readquirir mutex com profundidade preservada no cond_delete
    // ========================================================================
    {
        GuestScheduler sched(mem, 2);
        auto t_owner = sched.create(0x1000, 0x4000, 0x5000, 0x2000);
        auto t_waiter = sched.create(0x1000, 0x6000, 0x7000, 0x2000);
        CHECK(t_owner && t_waiter);
        CHECK(sched.start(t_owner) == ORBIS_OK && sched.start(t_waiter) == ORBIS_OK);

        // 1. Caso sem contenção: posse imediata e profundidade preservada (depth = 3)
        auto m = sched.mutex_create(true);
        auto c = sched.cond_create();

        // t_waiter adquire mutex com profundidade 3
        CHECK(sched.lock(t_waiter, m) == ORBIS_OK);
        CHECK(sched.lock(t_waiter, m) == ORBIS_OK);
        CHECK(sched.lock(t_waiter, m) == ORBIS_OK);
        CHECK(sched.mutex_depth(m) == 3);

        // t_waiter entra em cond_wait (libera mutex temporariamente)
        CHECK(sched.cond_wait(t_waiter, c, m) == ORBIS_OK);
        CHECK(sched.state(t_waiter) == GuestScheduler::State::waiting);
        CHECK(sched.mutex_depth(m) == 0);

        // cond_delete sem contenção: t_waiter readquire o mutex com profundidade 3 e acorda com ECANCELED
        CHECK(sched.cond_delete(c) == ORBIS_OK);
        CHECK(sched.state(t_waiter) == GuestScheduler::State::ready);
        CHECK(sched.resume(t_waiter) == ORBIS_KERNEL_ERROR_ECANCELED);
        CHECK(sched.mutex_depth(m) == 3);

        // Outro thread não pode desbloquear nem bloquear de imediato
        CHECK(sched.unlock(t_owner, m) == ORBIS_KERNEL_ERROR_EPERM);
        CHECK(sched.try_lock(t_owner, m) == ORBIS_KERNEL_ERROR_EBUSY);

        // t_waiter libera as 3 instâncias recursivas com sucesso
        CHECK(sched.unlock(t_waiter, m) == ORBIS_OK);
        CHECK(sched.mutex_depth(m) == 2);
        CHECK(sched.unlock(t_waiter, m) == ORBIS_OK);
        CHECK(sched.mutex_depth(m) == 1);
        CHECK(sched.unlock(t_waiter, m) == ORBIS_OK);
        CHECK(sched.mutex_depth(m) == 0);

        // 2. Caso com contenção: t_owner detém o mutex quando cond_delete ocorre
        auto c2 = sched.cond_create();
        CHECK(sched.lock(t_waiter, m) == ORBIS_OK);
        CHECK(sched.lock(t_waiter, m) == ORBIS_OK);
        CHECK(sched.mutex_depth(m) == 2);

        CHECK(sched.cond_wait(t_waiter, c2, m) == ORBIS_OK);
        CHECK(sched.state(t_waiter) == GuestScheduler::State::waiting);
        CHECK(sched.mutex_depth(m) == 0);

        // t_owner assume posse do mutex
        CHECK(sched.lock(t_owner, m) == ORBIS_OK);
        CHECK(sched.mutex_depth(m) == 1);

        // cond_delete com mutex em posse de t_owner: t_waiter entra em fila de readmissão
        CHECK(sched.cond_delete(c2) == ORBIS_OK);
        CHECK(sched.state(t_waiter) == GuestScheduler::State::waiting);
        CHECK(sched.mutex_depth(m) == 1);

        // t_owner libera: agora t_waiter recupera posse com profundidade original (2) e acorda com ECANCELED
        CHECK(sched.unlock(t_owner, m) == ORBIS_OK);
        CHECK(sched.state(t_waiter) == GuestScheduler::State::ready);
        CHECK(sched.resume(t_waiter) == ORBIS_KERNEL_ERROR_ECANCELED);
        CHECK(sched.mutex_depth(m) == 2);

        // Liberação por t_waiter
        CHECK(sched.unlock(t_waiter, m) == ORBIS_OK);
        CHECK(sched.unlock(t_waiter, m) == ORBIS_OK);
        CHECK(sched.mutex_depth(m) == 0);
        CHECK(sched.mutex_delete(m) == ORBIS_OK);
    }

    // ========================================================================
    // T175: Remover waiters de evento obsoletos em event_set (sem crescimento da fila)
    // ========================================================================
    {
        GuestScheduler sched(mem, 4);
        auto t_waiter1 = sched.create(0x1000, 0x4000, 0x5000, 0x2000);
        auto t_waiter2 = sched.create(0x1000, 0x6000, 0x7000, 0x2000);
        CHECK(t_waiter1 && t_waiter2);
        CHECK(sched.start(t_waiter1) == ORBIS_OK);
        CHECK(sched.start(t_waiter2) == ORBIS_OK);

        auto ev = sched.event_create();
        CHECK(sched.event_waiters_count(ev) == 0);

        // Ciclos repetidos de espera, timeout e sinalizacao
        for (int cycle = 0; cycle < 50; ++cycle) {
            // t_waiter1 aguarda bit 0x1 com timeout de 1000 us
            CHECK(sched.event_wait(t_waiter1, ev, 0x1, false, 1000) == ORBIS_OK);
            CHECK(sched.state(t_waiter1) == GuestScheduler::State::waiting);
            CHECK(sched.event_waiters_count(ev) == 1);

            // Relogio avanca alem do timeout: t_waiter1 expira
            sched.advance(2000);
            CHECK(sched.state(t_waiter1) == GuestScheduler::State::ready);
            CHECK(sched.resume(t_waiter1) == ORBIS_KERNEL_ERROR_ETIMEDOUT);

            // t_waiter2 aguarda bit 0x2 sem timeout
            CHECK(sched.event_wait(t_waiter2, ev, 0x2, false) == ORBIS_OK);
            CHECK(sched.state(t_waiter2) == GuestScheduler::State::waiting);

            // event_set sinaliza 0x2: acorda t_waiter2 e expurga t_waiter1 (obsoleto)
            CHECK(sched.event_set(ev, 0x2) == ORBIS_OK);
            CHECK(sched.state(t_waiter2) == GuestScheduler::State::ready);
            CHECK(sched.resume(t_waiter2) == ORBIS_OK);

            // Apos o expurgo pelo event_set, a fila nao cresce indefinidamente
            CHECK(sched.event_waiters_count(ev) == 0);
            CHECK(sched.event_clear(ev, 0x2) == ORBIS_OK);
        }

        CHECK(sched.event_delete(ev) == ORBIS_OK);
    }
    return 0;
}
