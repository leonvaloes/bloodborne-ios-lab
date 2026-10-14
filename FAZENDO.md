# FAZENDO — Atividades em Andamento Imediato

**Fase:** Integração Total Multi-Agente Concluída e Transição para a Sprint 5  
**Orquestrador:** AGENTE 5 (Integrador e Lead QA) — Pasta `bloodborne-ios-lab` (branch `main`)  
**Status da Suite de Testes:** 25/25 testes aprovados (100% verde) no host local e no CI (Ubuntu, Windows MSVC e Clang Sanitizers ASan/UBSan).
**Status do Smoke Test:** 118 verificações de baixo nível aprovadas com 0 falhas (`game_execution=false`).
**Status do Pipeline IPA:** Compilação e empacotamento iOS arm64 100% verde no CI GitHub Actions (`BBHost-unsigned-arm64` verificado com SHA-256 e Info.plist válido).

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

As quatro entregas da Sprint 5 foram concluídas, auditadas e integradas na `main`:
- **Agente 1 (VFS):** Confinamento estrito de caminhos de montagem via `weakly_canonical` absoluto, rejeitando fugas por symlink fora da raiz de Documents e tratamento robusto de erros de stat e componentes vazios.
- **Agente 2 (Metal):** Validação de malha FLVER antes da seleção no host Metal, checagem defensiva de buffers `nil` e telemetria honesta sem falsa alegação de apresentação na tela (`model_rendered=not_verified`).
- **Agente 3 (CPU/AOT):** Delimitação rigorosa dos bytes da sonda de entrada x86-64 ao segmento executável backed por arquivo, falhando de forma segura (fail-closed) sem inspecionar dados adjacentes nem simular boot.
- **Agente 4 (HLE):** Rejeição estrita de ponteiros guest inválidos de sincronização (semáforos, mutexes, condvars) antes de efeitos colaterais no host, com reversão de estado se a escrita de saída falhar.
- **Agente 5 (Integração e QA):** Resolução de discrepâncias de caminho Windows MSVC, eliminação de condição de corrida de capacidade em testes de semáforo sob sanitizers, validação cruzada 100% verde (25/25 suites, 118 checks) em Ubuntu, Windows MSVC e Sanitizers Clang. Worktrees sincronizados na HEAD `58221fa`.

O diagnóstico físico `bafe1c8` comprovou via VFS descompressão e parsing do pequeno DCX (1.024→2.992 bytes, 23 vértices/22 triângulos); Foundation e VFS concordaram que os outros três arquivos não estavam presentes naquela instalação. O commit `ce5941d` tenta selecionar essa malha no Metal ao abrir o app; ainda não foi validado no iPhone. `game_execution=false`. O critério para encerrar a Sprint 5 **não** é CI verde ou geometria isolada, mas menu, cena e controle derivados da execução da engine no iPhone, com evidências reproduzíveis.

Hipóteses antigas acima sobre montagem automática de todos os contêineres, shaders do jogo e loop de jogabilidade são descrições de plano/código parcial, não provas de boot do Bloodborne.
