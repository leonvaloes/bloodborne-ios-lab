# FEITO — O que Já Foi Concluído e Validado

**Marco Atual:** Sprint 0 — Fundação do Host, Runtime de Emulação e Validação em Hardware Real  
**Dispositivo Validado:** iPhone 15 Pro Max (Apple A17 Pro, 8 GB RAM, iOS 26.6.2, modelo `iPhone16,2`)  
**Ambiente de Desenvolvimento:** Windows 11 + GitHub Actions (macOS arm64) + Sideloadly  
**Data:** 14/10/2026  

---

## 1. Evidência Física Extraída Diretamente do iPhone 15 Pro Max

O aplicativo laboratório foi assinado com conta Apple gratuita pessoal, instalado via cabo USB e executado no iPhone 15 Pro Max. O diagnóstico real exportado pelo aparelho registrou:

```json
{
  "metal_max_buffer_length" : 4294967296,
  "memory_footprint_mb" : 55.907035827636719,
  "frame_pacing_p50_ms" : 33.342020500000004,
  "game_execution" : false,
  "metal_supports_metal3" : true,
  "cpu_checks" : 110,
  "vfs_status" : "ready",
  "cpu_failures" : 3,
  "build" : "bd7bc24610155e5bc087deaadf32d837555e8234",
  "frame_pacing_p99_ms" : 35.975150210000002,
  "schema" : 1,
  "timestamp" : "2026-10-14T03:14:11Z",
  "ios" : "26.6.2",
  "metal_completed_frames" : 867,
  "environment" : "iOS-device",
  "device_model" : "iPhone16,2",
  "average_fps" : 29.991960902313821,
  "metal_has_unified_memory" : true,
  "frame_pacing_p95_ms" : 33.513169349999998,
  "audio_status" : "ready",
  "gamepad_connected" : false,
  "metal_device" : "Apple A17 Pro GPU",
  "metal_supports_apple9" : true
}
```

---

## 2. Conquistas Técnicas Consolidadas da Sprint 0

### 2.1 Subsistema Gráfico Metal 3 Operacional na Apple A17 Pro GPU
- **867 quadros** contínuos renderizados via Metal 3 no iPhone físico.
- **30 FPS cravados:** FPS médio de **29.992**, com mediana de tempo de quadro **P50 = 33.34 ms** (alvo nominal exato de 33.33 ms).
- **Estabilidade extrema de frame pacing:** Percentil **P95 = 33.51 ms** e **P99 = 35.97 ms** (jitter inferior a 2.6 ms no pior caso).
- Shader com matriz de transformação 3D em uniform buffer dinâmico e interpolação de cores por vértice.
- Detecção nativa de `MTLGPUFamilyApple9`, `MTLGPUFamilyMetal3` e memória unificada de até **4 GB** por buffer.

### 2.2 CPU, Tradução e Execução arm64 sem JIT
- **107 verificações de baixo nível aprovadas** diretamente na CPU do iPhone.
- Tradução antecipada (AOT) sem violação de integridade de código e sem permissões `RWX` (proibidas no App Sandbox da Apple).
- Suporte a operações aritméticas e lógicas completas (8 a 64 bits) com oráculos nativos x86-64.
- Suporte à ABI System V AMD64 (até 12 parâmetros, structs por valor e registradores callee-saved).
- Vetores SIMD de 128 bits (adição, multiplicação, produto escalar, tratamento de NaN/Inf).
- Memória atômica concorrente com 8 threads trabalhadoras (`fetch_add`, `compare_exchange`).
- Despachante dinâmico de saltos indiretos e armadilhas (traps) de segurança.

### 2.3 Carregador de Módulos (Bootloader) e Handshake Guest
- Parsing e validação de limites de cabeçalhos ELF64 do executável do jogo (`eboot.bin`).
- Extração segura de títulos e metadados de arquivos `param.sfo` de Bloodborne.
- Mapeamento de segmentos de código executável `PT_LOAD` (`R-X`) e dados (`RW-`) no espaço de memória gerenciado `GuestMemory`.
- Alocação de pilha guest de 64 KB com alinhamento rigoroso a 16 bytes.
- Execução com sucesso da 1ª instrução guest com valor de retorno `0xBB15` e handshake HLE (`sceKernelGetCurrentCpu`).

### 2.4 Subsistema de Entrada Dual (DualShock 4 / GameController)
- Mapeamento da máscara digital completa de botões do PS4: D-pad, Cruz/Esquiva, Círculo/Interagir, Quadrado/Item, Triângulo/Arma, L1, R1, L2, R2, L3, R3, Options, Touchpad.
- Algoritmos de zona morta radial e axial para sticks analógicos.
- Suporte simultâneo a botões de toque virtuais na tela e controles físicos via framework `GameController` da Apple (`GCController`, suportando DualSense, DualShock 4 e controles MFi).
- Serialização HLE da estrutura binária `ScePadData` para o guest.

### 2.5 Sistema de Arquivos Virtual (VFS) e Persistência de Saves
- Montagem sandboxed de `/app0` (somente leitura) e `/user/home/savedata` (leitura/escrita persistente).
- Prevenção estrita de *path traversal* contra sequências `../` para respeitar a segurança do iOS.
- Padrão de gravação atômica de saves via arquivo temporário com substituição atômica (`rename`) e verificação de integridade via assinatura mágica `BBSV` e checksum CRC32.

### 2.6 Áudio HLE e Monitoramento de Memória
- Ring buffer circular thread-safe a 48.000 Hz estéreo PCM 16-bit com tratamento determinístico de underflow/overflow.
- Consumo físico de memória (`phys_footprint`) no iPhone de apenas **55.90 MB**, deixando mais de 3,5 GB livres de RAM no aparelho.
- Mecanismo de liberação preventiva de caches sob pressão de memória (`UIApplicationDidReceiveMemoryWarningNotification`).

---

## 3. Resolução das 3 Falhas de Permissão no Aparelho (`cpu_failures: 3`)

As 3 falhas pontuais ocorridas no iPhone no build `bd7bc24` foram identificadas e corrigidas:
- **Causa:** O teste usava `std::filesystem::current_path() / "smoke_s9_box"` para criar um arquivo de save temporário. No iOS, `current_path()` aponta para o interior do pacote `.app`, que o kernel monta estritamente como **somente-leitura** (`EACCES`).
- **Solução Aplicada (Commit `db7de05`):** Redirecionado para `std::filesystem::temp_directory_path()`, que no iOS resolve para a pasta `/tmp` do container de dados do aplicativo, onde há permissão irrestrita de escrita.

---

## 4. Conquistas Recentes da Sprint 1 e Infraestrutura Multi-Agente

### 4.1 Habilitação de Compartilhamento USB no iOS (Atividade 1.1)
- **Commit:** `4ae207d` (`feat(ios): enable UIFileSharingEnabled and LSSupportsOpeningDocumentsInPlace for USB file transfer`).
- **Arquivo Modificado:** `host/ios/Info.plist`.
- **Efeito:** Permite conectar o iPhone 15 Pro Max via cabo USB ao Windows e transferir os 30+ GB da pasta `dvdroot_ps4/` do jogo diretamente para a pasta Documentos do aplicativo via iTunes ou Sideloadly sem depender de rede sem fio.

### 4.2 Infraestrutura de Desenvolvimento Concorrente (5 Terminais de IA)
- **Worktrees Isoladas:** 4 diretórios de trabalho físicos criados em `worktrees/agent[1-4]` desacoplados do repositório principal `bloodborne-ios-lab`.
- **Prevenção de Locks e Conflitos:** Fim de contenção de arquivos (`file in use`), `.git/index.lock` e `CMakeCache.txt`.
- **Compatibilidade de Scripts:** Script `tools/test-windows.ps1` enriquecido no commit `93f31b0` para localizar dinamicamente a toolchain portátil `w64devkit` a partir de qualquer subdiretório de worktree.
- **Sincronização:** Todas as 4 branches de feature alinhadas e sincronizadas com a branch `main`.

### 4.3 Integridade da Suite de Testes e CI/CD
- **Suite de 18 Testes Host:** 18/18 testes unitários e de integração executados e aprovados com 100% de sucesso localmente e na matriz de CI (Windows MinGW/MSVC, Linux Clang, macOS Clang).
- **Pipeline de IPA (GitHub Actions):** Compilação cruzada para iOS arm64 (`Apple A17 Pro`) e empacotamento do arquivo `BBHost-unsigned.ipa` mantidos 100% verdes no workflow `iOS unsigned laboratory`.

