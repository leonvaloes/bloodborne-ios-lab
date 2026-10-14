# FAZENDO — Atividades em Andamento Imediato

**Fase:** Integração Total Multi-Agente Concluída e Transição para a Sprint 5  
**Orquestrador:** AGENTE 5 (Integrador e Lead QA) — Pasta `bloodborne-ios-lab` (branch `main`)  
**Status da Suite de Testes:** 22/22 testes aprovados (100% verde)  
**Status do Smoke Test:** 118 verificações de baixo nível aprovadas com 0 falhas  
**Status do Pipeline IPA:** Compilação e empacotamento iOS arm64 100% verde (`BBHost-unsigned-arm64`)  

---

## Matriz de Execução por Agente e Terminal

| Agente | Terminal / Pasta | Branch | Tarefas Realizadas | Status |
|:---:|---|---|---|:---:|
| **Agente 1** | `worktrees/agent1-vfs` | `feature/vfs-bdt-ingestion` | **Sprint 1 Concluída:** Parser de contêineres BHD5/BDT e descompressor DCX em streaming. | *Concluído / Integrado na main* |
| **Agente 2** | `worktrees/agent2-metal` | `feature/metal3-shaders` | **Sprint 2 Concluída:** Tradutor SPIR-V -> MSL 3.0, Argument Buffers Tier 2, Leitor FLVER2 e Pipeline 3D com Depth32Float e Blinn-Phong no host iOS. | *Concluído / Integrado na main* |
| **Agente 3** | `worktrees/agent3-aot` | `feature/aot-batch` | **Sprint 3 Concluída:** Lifter x86-64, Recompilador AOT em lote, Indirect Dispatcher com batch registry e validação no `eboot.elf` real. | *Concluído / Integrado na main* |
| **Agente 4** | `worktrees/agent4-hle` | `feature/hle-audio-threads` | **Sprint 4 Concluída:** Primitivas de multithreading do Orbis OS sobre pthreads e integração da LibAtrac9. | *Concluído / Integrado na main* |
| **Agente 5** | `bloodborne-ios-lab` | `main` | **Integração Contínua e QA:** Unificação total das 4 branches, compilação das 22 suítes de testes + smoke test com 118 verificações (100% aprovados), garantia do build IPA e sincronização dos boards. | *Concluído / 100% Verde* |

---

## Atividades em Execução Imediata (Sprint 5 — Jogo Jogável no iPhone):

### 1. Ingestão e Transferência de Dados via USB (Atividade 5.1)
- O suporte a compartilhamento de documentos via USB já está habilitado no `Info.plist` (`UIFileSharingEnabled = YES`).
- Os contêineres `.bhd5` e `.bdt` da pasta `dvdroot_ps4/` transferidos pelo iTunes ou Sideloadly são montados automaticamente pelo `VirtualFileSystem` em `/app0` com streaming aleatório em chunks de 64 KB.

### 2. Pipeline de Shaders e Geometria 3D no Host iOS (Atividade 5.2 e 5.3)
- O host nativo `host/ios/main.mm` já está compilado com o pipeline Metal 3 completo:
  - Shaders traduzidos de SPIR-V para MSL 3.0.
  - Formato de profundidade `Depth32Float` e render target `BGRA8Unorm` a 30 FPS.
  - Carregamento de malhas FLVER2 (mapas e personagens).

### 3. Loop de Jogabilidade e Entrada (Atividade 5.4)
- Mapeamento analógico do DualShock 4 e controles virtuais de toque na tela integrados ao motor para movimentação, esquiva e ataque.
