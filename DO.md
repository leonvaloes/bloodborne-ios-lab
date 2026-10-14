# DO — Backlog e Passo a Passo das Sprints até o Jogo Jogável

Este documento descreve o planejamento técnico dividido em entregas progressivas (**Sprints 1 a 5**) para transformar a fundação do host/runtime (concluída na **Sprint 0**) em uma experiência jogável de *Bloodborne* no iPhone 15 Pro Max.

---

## Sprint 1 — Ingestão de Dados e Leitor de Pacotes FromSoftware (`.bdt`/`.bhd`)
**Objetivo:** Permitir a transferência e leitura em streaming dos 30+ GB de dados de Bloodborne no iPhone.  
**Agente Responsável:** Agente 1 (worktrees/agent1-vfs — branch `feature/vfs-bdt-ingestion`)

- [x] **Atividade 1.1 — Compartilhamento de Arquivos USB via iTunes/Sideloadly:**
  - Configurar `UIFileSharingEnabled = YES` e `LSSupportsOpeningDocumentsInPlace = YES` no `host/ios/Info.plist`.
  - Permitir que o usuário conecte o iPhone por cabo e arraste a pasta `dvdroot_ps4/` do jogo diretamente para a pasta Documentos do app.
  - *(Concluído no commit `4ae207d` e validado no pipeline iOS)*
- [ ] **Atividade 1.2 — Leitor de Pacotes BHD5/BDT (FromSoftware Binder v4):**
  - Implementar o módulo `core/binder.cpp/.h` para parsing de cabeçalhos `.bhd5` (hashes de arquivos, offsets e tamanhos compactados).
  - Implementar streaming de dados em leitura aleatória sob demanda a partir dos arquivos `.bdt`.
- [ ] **Atividade 1.3 — Descompressor de Contêineres DCX (KRAK / Deflate / Oodle):**
  - Implementar descompressão de arquivos `.dcx` usados em texturas (`.tpf.dcx`), modelos (`.flver.dcx`) e mapas (`.msb.dcx`).
- [ ] **Atividade 1.4 — Teste Unitário de VFS com Dados Reais de Bloodborne:**
  - Validar a leitura e extração de parâmetros de armas (`gameparam.parambnd`) e textos de menu (`item.msgbnd`).
- **Critério de Conclusão da Sprint 1:** O app lê um arquivo `.bhd5`/`.bdt` real transferido para o iPhone e carrega um texto de item e uma textura em memória com 0 falhas.

---

## Sprint 2 — Tradução de Shaders e Pipeline Gráfico 3D (Vulkan/GNM → Metal 3)
**Objetivo:** Renderizar geometria tridimensional e iluminação de Bloodborne na GPU Apple A17 Pro.  
**Agente Responsável:** Agente 2 (worktrees/agent2-metal — branch `feature/metal3-shaders`)

- [ ] **Atividade 2.1 — Tradutor de Shaders (SPIR-V para Metal Shading Language):**
  - Integrar a biblioteca de tradução estática SPIRV-Cross para converter shaders compilados de PS4/Vulkan em MSL (Metal 3).
- [ ] **Atividade 2.2 — Gerenciamento de Descritores via Metal 3 Argument Buffers:**
  - Mapear tabelas de texturas e buffers de constantes de iluminação diretamente nos recursos da GPU Apple A17 Pro.
- [ ] **Atividade 2.3 — Leitor de Geometria de Modelos FLVER (`.flver`):**
  - Criar o parser dos modelos de Bloodborne (vértices, normais, coordenadas UV e ossos de animação).
  - Carregar os buffers de vértices diretamente na memória unificada Metal.
- [ ] **Atividade 2.4 — Configuração de Render Targets e Buffer de Profundidade:**
  - Configurar render targets HDR (RGBA16Float / RG11B10) e depth/stencil buffer para suportar oclusão de objetos 3D.
- **Critério de Conclusão da Sprint 2:** Um modelo 3D de Bloodborne (ex: a arma Saw Cleaver ou modelo do Caçador) é renderizado com textura e iluminação na tela do iPhone via Metal 3 a 30 FPS.

---

## Sprint 3 — Recompilação em Bloco do Executável do Jogo (`eboot.bin` / 84,85 MB)
**Objetivo:** Traduzir as seções de código do executável x86-64 do jogo para arm64 nativo.  
**Agente Responsável:** Agente 3 (worktrees/agent3-aot — branch `feature/aot-batch`)

- [ ] **Atividade 3.1 — Levantamento e Descompilação das Funções do Jogo (Binary Lifting):**
  - Usar análise estática (Ghidra / LLVM) para isolar as funções de inicialização, física (Havok), lógica de jogo e scripts.
- [ ] **Atividade 3.2 — Recompilador AOT em Lote:**
  - Expandir o gerador `bb-aot` para traduzir blocos de milhares de funções do código PS4 em C++ estruturado compatível com arm64.
- [ ] **Atividade 3.3 — Tabela Central de Despacho de Saltos Indiretos:**
  - Conectar todos os ponteiros de função de vtables e callbacks do jogo ao `IndirectDispatcher`.
- [ ] **Atividade 3.4 — Execução do Fluxo de Boot da Engine FromSoftware:**
  - Avançar a execução para além do handshake inicial, executando as rotinas de inicialização de memória e heaps do jogo.
- **Critério de Conclusão da Sprint 3:** O código do jogo executa a inicialização do motor FromSoftware no iPhone e chega à rotina de carregamento dos arquivos de configuração inicial.

---

## Sprint 4 — HLE das Bibliotecas do Sistema PS4 (`libkernel`, `libpad`, `libvideo`, `libaudio`)
**Objetivo:** Emular o sistema operacional do PS4 para permitir que o jogo toque sons, vídeos e gerencie threads.  
**Agente Responsável:** Agente 4 (worktrees/agent4-hle — branch `feature/hle-audio-threads`)

- [x] **Atividade 4.1 — Primitivas de Concorrência e Multithreading:** (Concluído pelo Agente 4)
  - Implementar os semáforos, mutexes leves e variáveis de condição do Orbis OS sobre o subsistema pthreads do iOS (`core/kernel_threads.h/.cpp`).
- [x] **Atividade 4.2 — Decodificação de Áudio ATRAC9:** (Concluído pelo Agente 4)
  - Integrar o descompressor LibAtrac9 para converter os streams de áudio do jogo para o ring buffer PCM de 48.000 Hz (`third_party/LibAtrac9/`, `core/atrac9_decoder.h/.cpp`, `core/audio.h/.cpp`).
- [ ] **Atividade 4.3 — Cinemática de Abertura:**
  - Implementar reprodutor de vídeo H.264 integrado ao Metal para exibir o vídeo de introdução da FromSoftware.
- [ ] **Atividade 4.4 — Orçamento de Memória e Garbage Collection:**
  - Manter o consumo total da aplicação abaixo de 3,5 GB de RAM para garantir imunidade contra o watchdog Jetsam do iOS.
- **Critério de Conclusão da Sprint 4:** O jogo exibe o vídeo de introdução com áudio estéreo sincronizado e inicializa todas as threads sem congelamentos.

---

## Sprint 5 — Tela de Título, Menus e Primeira Cena Jogável (Clínica de Iosefka)
**Objetivo:** Permitir interação completa com o menu, início de nova campanha e controle do Caçador na primeira sala.

- [ ] **Atividade 5.1 — Renderização de Interface e Menus 2D:**
  - Renderizar a tela de título ("Pressione Qualquer Botão"), menus de seleção de opções e tela de criação de personagem.
- [ ] **Atividade 5.2 — Carregamento do Mapa `m10_00_00_00` (Clínica de Iosefka):**
  - Carregar o mapa inicial, as fontes de luz da sala, a névoa volumétrica e a posição de spawn do Caçador.
- [ ] **Atividade 5.3 — Integração do Loop de Jogabilidade:**
  - Permitir movimentação com direcional analógico, esquiva com botão Cruz, ataque leve (R1) e câmera com analógico direito.
- [ ] **Atividade 5.4 — Frame Pacing e Otimização Térmica:**
  - Garantir taxa constante de 30 FPS (P50 ~33.3 ms) durante a exploração da primeira sala com registro de temperatura.
- **Critério de Conclusão da Sprint 5:** O usuário consegue iniciar um "Novo Jogo" no iPhone 15 Pro Max e controlar o Caçador caminhando e atacando na Clínica de Iosefka com gráficos 3D a 30 FPS estáveis.
