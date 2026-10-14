# DO — Backlog e Passo a Passo das Sprints até o Jogo Jogável

Este documento descreve o planejamento técnico dividido em entregas progressivas (**Sprints 0 a 5**) para transformar a fundação do host/runtime em uma experiência jogável de *Bloodborne* no iPhone 15 Pro Max.

---

## Sprint 0 — Fundação do Host, Runtime de Emulação e Validação em Hardware Real
**Objetivo:** Estabelecer o laboratório iOS com renderização Metal 3 a 30 FPS, memória unificada, despachante dinâmico e execução no hardware físico sem Mac próprio.  
**Status:** **Concluído e Validado no Aparelho** (Build `bd7bc24` — iPhone 15 Pro Max, iOS 26.6.2)

- [x] **Atividade 0.1 — Subsistema Gráfico Metal 3 na GPU Apple A17 Pro:**
  - 867 quadros contínuos a 29.992 FPS com frame pacing P50 = 33.34 ms e jitter inferior a 2.6 ms (P99 = 35.97 ms).
- [x] **Atividade 0.2 — CPU e ABI SysV x86-64 sobre arm64:**
  - 107 verificações aprovadas diretamente no aparelho sem uso de JIT (App Sandbox 100% respeitado).
- [x] **Atividade 0.3 — Virtual File System (VFS) e Persistência de Saves:**
  - Montagem de `/app0` (somente-leitura) e `/tmp` para escritas transitórias; saves atômicos com magic `BBSV` e CRC32.
- [x] **Atividade 0.4 — Pipeline de CI/CD Cruzado com GitHub Actions:**
  - Build automatizado no macOS arm64 (`Apple A17 Pro`) gerando artefato IPA sem assinatura (`BBHost-unsigned.ipa`).

---

## Sprint 1 — Ingestão de Dados e Leitor de Pacotes FromSoftware (`.bdt`/`.bhd`)
**Objetivo:** Permitir a transferência e leitura em streaming dos 30+ GB de dados de Bloodborne no iPhone.  
**Agente Responsável:** Agente 1 (worktree `agent1-vfs` — branch `feature/vfs-bdt-ingestion`)  
**Status:** **Concluído e Integrado na Main** (Commit `04e4c85`)

- [x] **Atividade 1.1 — Compartilhamento de Arquivos USB via iTunes/Sideloadly:**
  - Configurado `UIFileSharingEnabled = YES` e `LSSupportsOpeningDocumentsInPlace = YES` no `host/ios/Info.plist`.
  - Permite transferência direta da pasta `dvdroot_ps4/` por cabo USB sem depender de Wi-Fi lento.
- [x] **Atividade 1.2 — Leitor de Pacotes BHD5/BDT (FromSoftware Binder v4):**
  - Implementado `core/binder.h/.cpp` com leitura de cabeçalhos BHD5, hashing FromSoftware de paths e buckets indexados.
  - Streaming sob demanda a partir dos arquivos `.bdt` sem alocar 30 GB na RAM do iPhone.
- [x] **Atividade 1.3 — Descompressor de Contêineres DCX (KRAK / Deflate / DFLT):**
  - Implementada descompressão em streaming via rotinas otimizadas de `miniz` (`tinfl_decompress`).
- [x] **Atividade 1.4 — Teste Unitário de VFS com Ingestão de Dados:**
  - Implementado `tests/test_binder.cpp` com 100% de aprovação na extração de contêineres e hashes.

---

## Sprint 2 — Tradução de Shaders e Pipeline Gráfico 3D (Vulkan/GNM → Metal 3)
**Objetivo:** Renderizar geometria tridimensional e iluminação de Bloodborne na GPU Apple A17 Pro.  
**Agente Responsável:** Agente 2 (worktree `agent2-metal` — branch `feature/metal3-shaders`)  
**Status:** **Concluído e Integrado na Main** (Commit `6e5fcf5`)

- [x] **Atividade 2.1 — Tradutor de Shaders (SPIR-V para Metal Shading Language 3.0):**
  - Módulo `core/shader_translator.h/.cpp` com reflexão completa de bytecode SPIR-V, inversão de eixo Y (clip space) e alinhamento de uniforms.
- [x] **Atividade 2.2 — Gerenciamento de Descritores via Metal 3 Argument Buffers:**
  - Módulo `core/metal_pipeline.h/.cpp` com Argument Buffers Tier 2 (`Metal3ArgumentTable`), mapeando até 32 texturas e 8 samplers.
  - Biblioteca matemática 3D com projeção perspectiva, matriz normal e uniforms de iluminação Blinn-Phong.
- [x] **Atividade 2.3 — Leitor de Geometria de Modelos FLVER2 (`.flver`/`.dcx`):**
  - Módulo `core/flver_parser.h/.cpp` com decodificação de vértices, normais, tangentes, UVs, ossos, submeshes e triangle strip desdobrado.
  - Conversão direta para buffers Metal indexados (`MetalMeshBuffer`) e suporte ao modelo FromSoftware Saw Cleaver e mapas reais.
- [x] **Atividade 2.4 — Render Targets HDR, Depth Buffer e Host iOS 3D:**
  - Formatos HDR e buffer de profundidade `Depth32Float` integrados a `host/ios/main.mm` com renderização 3D a 30 FPS.

---

## Sprint 3 — Recompilação em Bloco do Executável do Jogo (`eboot.bin` / 84,85 MB)
**Objetivo:** Traduzir as seções de código do executável x86-64 do jogo para arm64 nativo.  
**Agente Responsável:** Agente 3 (worktree `agent3-aot` — branch `feature/aot-batch`)  
**Status:** **Concluído e Integrado na Main** (Commit `6066d90`)

- [x] **Atividade 3.1 — Levantamento e Descompilação das Funções do Jogo (Binary Lifting):**
  - Decodificador de instruções x86-64 completo em `tools/aot.h/.cpp` (ALU, MOV, jumps, chamadas, flags).
- [x] **Atividade 3.2 — Recompilador AOT em Lote:**
  - Varredura de segmentos `PT_LOAD` (`R-X`) do `eboot.elf` real gerando lotes C++ estruturados sem violação de Sandboxing.
- [x] **Atividade 3.3 — Tabela Central de Despacho de Saltos Indiretos:**
  - `IndirectDispatcher::register_batch` em `core/dispatcher.h/.cpp` conectando vtables e chamadas indiretas.
- [x] **Atividade 3.4 — Execução do Fluxo de Boot da Engine FromSoftware:**
  - Varredura e levantamento do ponto de entrada real `0xa0` (67 instruções, 3 blocos) do executável real do jogo.

---

## Sprint 4 — HLE das Bibliotecas do Sistema PS4 (`libkernel`, `libpad`, `libaudio`)
**Objetivo:** Emular o sistema operacional do PS4 para gerenciar threads, controle e áudio ATRAC9.  
**Agente Responsável:** Agente 4 (worktree `agent4-hle` — branch `feature/hle-audio-threads`)  
**Status:** **Concluído e Integrado na Main** (Commit `c5372e5`)

- [x] **Atividade 4.1 — Primitivas de Concorrência e Multithreading:**
  - Semáforos contados, mutexes leves com fast-path atômico e condvars sobre pthreads em `core/kernel_threads.h/.cpp`.
  - Mapeamento completo de NIDs Orbis no despachante HLE (`core/hle.cpp`).
- [x] **Atividade 4.2 — Decodificação de Áudio ATRAC9 com LibAtrac9:**
  - Integração da biblioteca C99 `LibAtrac9` (`third_party/LibAtrac9/`), classe `Atrac9Decoder` e streaming para ring buffer estéreo a 48.000 Hz.
- [x] **Atividade 4.3 — Subsistema de Entrada Dual (DualShock 4 / GameController):**
  - Mapeamento digital de todos os botões e sticks analógicos com zona morta radial e integração a `GCController`.
- [x] **Atividade 4.4 — Orçamento de Memória e Monitoramento Térmico:**
  - Gestão de memória física com pegada de apenas 55.9 MB e liberação preventiva de caches sob pressão do iOS.

---

## Sprint 5 — Tela de Título, Menus e Primeira Cena Jogável (Clínica de Iosefka)
**Objetivo:** Permitir interação completa com o menu, início de nova campanha e controle do Caçador na primeira sala.  
**Status:** **Em Execução Imediata / Próxima Entrega Física**

- [ ] **Atividade 5.1 — Transferência e Ingestão dos Assets do Jogo no Aparelho:**
  - Conectar o iPhone 15 Pro Max por USB e transferir os arquivos de dados `dvdroot_ps4/` para a pasta Documentos do app.
- [ ] **Atividade 5.2 — Renderização de Interface e Menus 2D:**
  - Exibir a tela de título ("Pressione Qualquer Botão"), menu de seleção e fluxo de início de campanha.
- [ ] **Atividade 5.3 — Carregamento do Mapa Inicial `m10_00_00_00` (Clínica de Iosefka):**
  - Carregamento de malhas FLVER2 do cenário, iluminação ambiente/direcional Blinn-Phong e spawn do Caçador.
- [ ] **Atividade 5.4 — Controle Completo do Personagem e Frame Pacing 30 FPS:**
  - Movimentação com analógico virtual/físico, esquiva (Cruz), ataque (R1) e câmera fluida a 30 FPS estáveis.
- **Critério de Conclusão da Sprint 5:** O usuário consegue controlar o Caçador caminhando e atacando na Clínica de Iosefka diretamente no iPhone 15 Pro Max.
