# Bloodborne iOS Lab — Host e Runtime de Pesquisa arm64

Laboratório de engenharia reversa, emulação e pesquisa para execução de Bloodborne no iPhone 15 Pro Max (Apple A17 Pro, arm64, iOS 26.6), desenvolvido em ambiente Windows 11 com automação de compilação remota via GitHub Actions e sideload pessoal.

---

## Estado de Desenvolvimento (Marcos S0–S9 Concluídos)

- **S0 — Inventário e Estrutura:** 709 arquivos inventariados com SHA-256 em `docs/upstream-files.csv` e guia estrutural detalhado em `docs/PORT_PC_STRUCTURE.md`.
- **S1 — Testes Nativos no Host:** Testes unitários e de integração passando 100% no Windows MSVC/MinGW, Linux (com sanitizers ASan/UBSan) e macOS Clang.
- **S2 — Compilação arm64 iOS:** Pipeline de compilação para SDK `iphoneos` gerando pacote `BBHost-unsigned.ipa` no CI.
- **S3 — Validação em Dispositivo Real:** Instalação e execução validadas em iPhone 15 Pro Max físico (14 verificações CPU, 0 falhas, 471 frames Metal).
- **S4 — Tradução AOT x86-64 → arm64:** 37 fixtures cobrindo aritmética 8–64b, chamadas diretas/indiretas, recursão, jump tables e ABI completa. Validado fisicamente no aparelho com 49 verificações e 0 falhas.
- **S5 — ABI, SIMD e Concorrência:** Suporte vetorial SIMD 128-bit (F4), atomics com memória concorrente (F5), despachante de alvos indiretos (F6) e chamadas HLE de kernel.
- **S6 — Pipeline Gráfico Metal 3:** Renderizador Metal 3 com uniform buffer dinâmico de rotação a 30 FPS, interpolação de cores por vértice e detecção de GPU Apple A17 Pro.
- **S7 — Parser Seguro de Executáveis:** Parser com bounds checking para extração de metadados de arquivos `param.sfo` e headers ELF64 (`eboot.bin`).
- **S8 — Carregador de Módulos e 1ª Instrução:** Mapeamento de segmentos `PT_LOAD` no espaço `GuestMemory` sem violação RWX, pilha guest de 64 KB alinhada a 16 bytes e execução da 1ª instrução guest.
- **S9 — Jogabilidade, VFS, Áudio e Estabilidade:** Subsistemas completos de entrada (DualShock 4, zona morta radial, GameController Apple MFi/DualSense e touch virtual), VFS com isolamento sandboxed para `/app0` e `/user/home/savedata`, persistência atômica com integridade CRC32, subsistema de áudio HLE (48 kHz estéreo) e medição de estabilidade e frame pacing (P50, P95, P99) com vigilância de `phys_footprint`.

---

## Compilação e Execução de Testes Locais

Para compilar e executar a suite completa de 18 testes unitários e de integração no Windows:

```powershell
$env:PATH = "C:\Users\Usuario\Documents\bloodborne_iphone\.tools\w64devkit\bin;" + $env:PATH
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
ctest --test-dir build --output-on-failure
```

Para executar o verificador integrado (`bb-smoke.exe`):

```powershell
.\build\bb-smoke.exe
```

Saída esperada:
```json
{"schema":1,"build":"local-unversioned","environment":"host","checks":110,"failures":0,"game_execution":false}
```

---

## Documentação dos Marcos

Todos os relatórios de validação técnica, evidências e logs de cada marco estão disponíveis na pasta `docs/milestones/`:
- [S3 — Validação em Dispositivo Físico](docs/milestones/S3_DEVICE_VALIDATION.md)
- [S4 — Expansão de Tradução e Validação Física](docs/milestones/S4_TRANSLATION_EXPANSION.md)
- [S5 — ABI, Dispatch e Concorrência](docs/milestones/S5_ABI_DISPATCH_CONCURRENCY.md)
- [S6 — Pipeline Gráfico Metal 3](docs/milestones/S6_METAL_PIPELINE.md)
- [S7 — Parser Seguro de Metadados e ELF](docs/milestones/S7_SAFE_GAME_PARSER.md)
- [S8 — Carregamento de Módulos e Execução Inicial](docs/milestones/S8_GUEST_BOOT.md)
- [S9 — Jogabilidade, Áudio, Entrada, VFS e Estabilidade](docs/milestones/S9_GAMEPLAY_STABILITY_SUBSYSTEMS.md)
- [Relatório Consolidado de Engenharia (S0–S9)](docs/CONSOLIDATED_PROJECT_REPORT.md)
