# FAZENDO — Atividades em Andamento Imediato

**Fase:** Integração Total Multi-Agente Concluída e Transição para a Sprint 5  
**Orquestrador:** AGENTE 5 (Integrador e Lead QA) — Pasta `bloodborne-ios-lab` (branch `main`)  
**Status da Suite de Testes:** 25/25 testes host locais após integração das quatro frentes; CI dessa integração ainda pendente.
**Status do Smoke Test:** 118 verificações de baixo nível aprovadas com 0 falhas (`game_execution=false`).
**Status do Pipeline IPA:** execução iOS de `ce5941d` foi cancelada sem iniciar o runner macOS; último IPA validado no CI é `bafe1c8` (sonda de geometria, não jogo).

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

## Sprint 5 — ciclo de cinco frentes (14/10/2026)

As quatro worktrees foram encontradas limpas, porém suas branches antigas já foram integradas; cada frente deve sincronizar `main` antes de novo trabalho. Agentes atuam **somente** em sua worktree, sem push ou merge em `main`. O Agente 5 revisa commits, testa a integração e atualiza os quadros. Não se repetem as Sprints 1–4.

| Frente | Entrega pequena em andamento | Aceite antes de integrar |
|---|---|---|
| 1 — VFS | Robustez da ingestão Documents/DCX | testes sintéticos de limites e falha segura; nenhum arquivo comercial publicado |
| 2 — Metal | Robustez/telemetria da seleção do FLVER real | CI iOS e indicação honesta de fallback; teste visual em aparelho ainda pendente |
| 3 — CPU/AOT | Caracterização verificável dos limites da sonda de entrada | fixtures autorais e rejeição de opcode desconhecido; não chamar duas instruções de boot |
| 4 — HLE | Lacuna pequena de HLE/threads com teste host | regressão sintética, sem alegar chamadas reais do jogo |
| 5 — integração | Revisar alterações e CI, resolver conflitos, manter boards | Git limpo, testes Windows/CI separados e evidência física distinta |

As quatro entregas foram integradas localmente nos commits `fc38a81` (sonda CPU com limite de segmento), `155810f` (confinamento de symlink VFS), `6161d1e` (validação de malha/telemetria Metal) e `ce02fe1` (ponteiros HLE). Testes Windows de integração: 25/25 e smoke 118/0. Ainda não houve push dessa integração nem build iOS novo; o último CI de `ce5941d` falhou por cancelamento de jobs macOS **antes de executar etapas**, não por erro de compilação. O CI de host Linux/Windows/sanitizers desse commit passou, mas o workflow geral ficou vermelho.

O diagnóstico físico `bafe1c8` comprovou via VFS descompressão e parsing do pequeno DCX (1.024→2.992 bytes, 23 vértices/22 triângulos); Foundation e VFS concordaram que os outros três arquivos não estavam presentes naquela instalação. O commit `ce5941d` tenta selecionar essa malha no Metal ao abrir o app; ainda não foi validado no iPhone. `game_execution=false`. O critério para encerrar a Sprint 5 **não** é CI verde ou geometria isolada, mas menu, cena e controle derivados da execução da engine no iPhone, com evidências reproduzíveis.

Hipóteses antigas acima sobre montagem automática de todos os contêineres, shaders do jogo e loop de jogabilidade são descrições de plano/código parcial, não provas de boot do Bloodborne.
