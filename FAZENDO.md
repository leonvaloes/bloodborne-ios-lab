# FAZENDO — Atividades em Andamento Imediato

**Fase:** Execução Paralela Multi-Agente (Sprints 1 a 4 em branches dedicadas)  
**Orquestrador:** AGENTE 5 (Integrador e Lead QA) — Pasta `bloodborne-ios-lab` (branch `main`)  
**Status da Suite de Testes:** 18/18 testes aprovados (100% verde)  
**Status do Pipeline IPA:** Compilação e empacotamento iOS arm64 100% verde (`BBHost-unsigned-arm64`)  

---

## Matriz de Execução por Agente e Terminal

| Agente | Terminal / Pasta | Branch | Tarefas em Andamento | Status |
|:---:|---|---|---|:---:|
| **Agente 1** | `worktrees/agent1-vfs` | `feature/vfs-bdt-ingestion` | **Atividades 1.2 e 1.3:** Implementação de `core/binder.h/.cpp` (BHD5/BDT parser) e descompressor DCX em streaming. | *Em execução* |
| **Agente 2** | `worktrees/agent2-metal` | `feature/metal3-shaders` | **Atividades 2.1 e 2.3:** Integração SPIRV-Cross para Metal MSL e parser de malhas 3D FLVER (`.flver`). | *Em execução* |
| **Agente 3** | `worktrees/agent3-aot` | `feature/aot-batch` | **Atividades 3.1 e 3.2:** Expansão do gerador `bb-aot` para recompilação estática em lote de funções do executável PS4. | *Em execução* |
| **Agente 4** | `worktrees/agent4-hle` | `feature/hle-audio-threads` | **Atividades 4.1 e 4.2:** Primitivas de multithreading do Orbis OS sobre pthreads e integração da LibAtrac9. | *Em execução* |
| **Agente 5** | `bloodborne-ios-lab` | `main` | **Integração Contínua e QA:** Gestão do board, revisão de commits dos agentes 1 a 4, merge das features validadas, execução da suite de 18 testes e garantia do build IPA. | *Ativo / 100% Verde* |

---

## Detalhamento das Tarefas em Andamento:

### 1. Terminal 1 (Agente 1) — Leitor BHD5/BDT e Descompressor DCX
- **Módulo:** `core/binder.h` e `core/binder.cpp`.
- **Objetivo:** O Bloodborne armazena seus assets em contêineres `.bdt` indexados por `.bhd5`. O leitor precisa realizar streaming sob demanda de arquivos específicos diretamente para o VFS sem carregar 30 GB na RAM.
- **Descompressão DCX:** Suporte a arquivos compactados (KRAK / Deflate) para extração transparente de texturas e modelos.

### 2. Terminal 2 (Agente 2) — Tradutor de Shaders SPIR-V e Geometria FLVER
- **Módulos:** `core/shader_translator.h` e `core/flver_parser.h`.
- **Objetivo:** Traduzir shaders compilados PS4/Vulkan para Metal Shading Language (MSL) e carregar a geometria dos modelos do jogo na GPU Apple A17 Pro com layout de memória unificada.

### 3. Terminal 3 (Agente 3) — Recompilador AOT em Lote (x86-64 → arm64)
- **Módulo:** `tools/aot.cpp` e `tools/aot_main.cpp`.
- **Objetivo:** Recompilar em C++ estático as seções de código do executável `eboot.elf` (84,85 MB), gerando blocos sem dependência de JIT ou permissões proibidas `RWX`.

### 4. Terminal 4 (Agente 4) — Multithreading Orbis OS e Áudio ATRAC9
- **Módulos:** `core/kernel_threads.h` e integração da LibAtrac9.
- **Objetivo:** Emular chamadas de semáforos/mutexes leves (`sceKernelCreateSema`, `sceKernelWaitSema`) e alimentar o ring buffer estéreo de 48.000 Hz com pacotes decodificados.

### 5. Terminal 5 (Agente 5) — Orquestração de QA e Integridade do Pipeline
- **Branches Sincronizadas:** As 4 branches de feature (`feature/vfs-bdt-ingestion`, `feature/metal3-shaders`, `feature/aot-batch`, `feature/hle-audio-threads`) foram sincronizadas com o commit mais recente da `main` (`93f31b0`).
- **Validação Local:** Script `tools/test-windows.ps1` ajustado para execução transparente a partir de qualquer worktree.
- **Suite de 18 Testes:** Execução contínua com 100% de aprovação (tempo de execução: ~2.2 s no host Windows).
- **Pipeline Remoto de IPA:** GitHub Actions monitorado em tempo real com compilação e artefato `BBHost-unsigned-arm64` ativo.
