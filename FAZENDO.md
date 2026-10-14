# FAZENDO — Atividades em Andamento Imediato

**Fase:** Integracao Total Multi-Agente Concluida e Validacao Lead QA Final  
**Orquestrador:** AGENTE 5 (Integrador e Lead QA) — Pasta `bloodborne-ios-lab` (branch `main`)  
**Status da Suite de Testes:** 21/21 testes aprovados (100% verde)  
**Status do Pipeline IPA:** Compilacao e empacotamento iOS arm64 100% verde (`BBHost-unsigned-arm64`)  

---

## Matriz de Execucao por Agente e Terminal

| Agente | Terminal / Pasta | Branch | Tarefas Realizadas | Status |
|:---:|---|---|---|:---:|
| **Agente 1** | `worktrees/agent1-vfs` | `feature/vfs-bdt-ingestion` | **Sprint 1 Concluida:** Parser de contêineres BHD5/BDT e descompressor DCX em streaming. | *Concluido / Integrado na main* |
| **Agente 2** | `worktrees/agent2-metal` | `feature/metal3-shaders` | **Sprint 2 Concluida:** Tradutor SPIR-V -> MSL 3.0, Argument Buffers Tier 2, Leitor FLVER2 e Pipeline 3D com Depth32Float e Blinn-Phong no host iOS. | *Concluido / Integrado na main* |
| **Agente 3** | `worktrees/agent3-aot` | `feature/aot-batch` | **Sprint 3 Concluida:** Lifter x86-64, Recompilador AOT em lote, Indirect Dispatcher com batch registry e validacao no `eboot.elf` real. | *Concluido / Integrado na main* |
| **Agente 4** | `worktrees/agent4-hle` | `feature/hle-audio-threads` | **Sprint 4 Concluida:** Primitivas de multithreading do Orbis OS sobre pthreads e integracao da LibAtrac9. | *Concluido / Integrado na main* |
| **Agente 5** | `bloodborne-ios-lab` | `main` | **Integracao Continua e QA:** Unificacao total das 4 branches, compilacao das 21 suites de testes + smoke test com 118 verificacoes, garantia do build IPA e documentacao por sprints. | *Concluido / 100% Verde* |

---

## Proximas Atividades Imediatas (Sprint 5 / Empacotamento de Assets):
1. **Transferencia dos Assets do Jogo:** Conectar o iPhone 15 Pro Max ao Windows via USB e copiar a pasta `dvdroot_ps4/` extraida de Bloodborne para a pasta de documentos do app via iTunes ou Sideloadly.
2. **Execucao do Pipeline AOT Completo:** Rodar `bb-aot --batch-elf` para gerar todas as unidades de traducao C++ do `eboot.bin` completo do jogo.
3. **Execucao e Jogabilidade em Hardware:** Inicializar o aplicativo no iPhone 15 Pro Max com os dados montados no VFS e validar o boot completo do jogo.
