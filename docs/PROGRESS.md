# Progresso Verific?vel ? Laborat?rio Bloodborne iOS

Atualiza??o: **14/10/2026**. Este documento registra o status dos marcos do projeto com base em evid?ncias verific?veis.

| Marco | Estado | Evid?ncia Principal |
|---|---|---|
| **S0** ? Fontes, revis?es e invent?rio | ? Conclu?do | 709 arquivos inventariados com SHA-256 e categoriza??o em `docs/upstream-files.csv`. Subm?dulos catalogados. |
| **S1** ? Testes host e sanitizers | ? Conclu?do | Testes automatizados rodando em Windows (MSVC/GCC), Ubuntu (ASan/UBSan) e macOS (Clang). |
| **S2** ? Build iOS arm64 no CI | ? Conclu?do | Compila??o com Xcode SDK `iphoneos` em runner macOS, gerando `BBHost-unsigned.ipa` arm64. |
| **S3** ? Execu??o no iPhone f?sico | ? Conclu?do | App instalado via sideload no iPhone 15 Pro Max (iOS 26.6.2, A17 Pro GPU). Diagn?stico: 14 checks CPU/mem?ria, 0 falhas, 471 frames Metal. |
| **S4** ? Expans?o de tradu??o est?tica | ? Conclu?do e Validado em Hardware | 37 fixtures autorais (F1, F2, F3) cobrindo aritm?tica 8?64 bits, ADC/SBB/CMP/TEST, chamadas diretas/indiretas, recurs?o, jump tables, ABI 0?12 args, structs e FP. **Validado no iPhone 15 Pro Max: 49 checks, 0 falhas, 189 frames Metal**. |
| **S5** ? ABI, saltos indiretos, mem?ria guest | ? Conclu?do | F4 (SIMD 128-bit), F5 (atomics LOCK XADD/CMPXCHG com 8 threads concorrentes), F6 (despacho indireto din?mico e HLE imports `libkernel`) e valida??o de manifesto JSON. CI validado em macOS/Windows/Linux. |
| **S6** ? Pipeline gr?fico representativo | ? Conclu?do | Pipeline Metal 3 a 30 FPS no A17 Pro com uniform buffer din?mico de rota??o, interpola??o de cores por v?rtice e query de GPU capabilities (`MTLGPUFamilyApple9`, `MTLGPUFamilyMetal3`). |
| **S7** ? Parser seguro do jogo | ? Conclu?do | Parser seguro de metadados (`param.sfo` -> Bloodborne CUSA03173) e cabe?alhos de execut?vel ELF/SELF (`eboot.bin` / `eboot.elf`) com verifica??o estrita de bounds e overflow. |
| **S8** ? Boot e primeira instru??o guest | ? Conclu?do | Mapeamento dos segmentos `PT_LOAD` do jogo para o espa?o de mem?ria guest, inicializa??o de pilha (alinhada a 16 bytes), contexto de CPU guest e execu??o da primeira instru??o com 0 falhas. **13/13 testes aprovados, 79 checks no smoke**. |
| **S9** ? Jogabilidade e estabilidade | ?? Em Progresso | Mapeamento de controles de toque / gamepad Bluetooth, ?udio e perfil de estabilidade t?rmica/mem?ria. |

---

*Nota de integridade: A conclus?o dos marcos S0?S8 demonstra a maturidade da infraestrutura, do subsistema de mem?ria virtual, despacho din?mico, renderiza??o Metal 3, parser de arquivos do jogo e boot de instru??o guest com valida??o emp?rica.*
