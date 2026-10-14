# Progresso Verific?vel ? Laborat?rio Bloodborne iOS

Atualiza??o: **14/10/2026**. Este documento registra o status dos marcos do projeto com base em evid?ncias verific?veis.

| Marco | Estado | Evid?ncia Principal |
|---|---|---|
| **S0** ? Fontes, revis?es e invent?rio | ? Conclu?do | 709 arquivos inventariados com SHA-256 e categoriza??o em `docs/upstream-files.csv`. Subm?dulos catalogados. |
| **S1** ? Testes host e sanitizers | ? Conclu?do | Testes automatizados rodando em Windows (MSVC/GCC), Ubuntu (ASan/UBSan) e macOS (Clang). |
| **S2** ? Build iOS arm64 no CI | ? Conclu?do | Compila??o com Xcode SDK `iphoneos` em runner macOS, gerando `BBHost-unsigned.ipa` arm64. |
| **S3** ? Execu??o no iPhone f?sico | ? Conclu?do | App instalado via sideload no iPhone 15 Pro Max (iOS 26.6.2, A17 Pro GPU). Diagn?stico: 14 checks CPU/mem?ria, 0 falhas, 471 frames Metal. |
| **S4** ? Expans?o de tradu??o est?tica | ? Conclu?do e Validado em Hardware | 37 fixtures autorais (F1, F2, F3) cobrindo aritm?tica 8?64 bits, ADC/SBB/CMP/TEST, chamadas diretas/indiretas, recurs?o, jump tables, ABI 0?12 args, structs e FP. **Validado no iPhone 15 Pro Max: 49 checks, 0 falhas, 189 frames Metal**. |
| **S5** ? ABI, saltos indiretos, mem?ria guest | ? Conclu?do no Host | F4 (SIMD 128-bit), F5 (atomics LOCK XADD/CMPXCHG com 8 threads concorrentes), F6 (despacho indireto din?mico e HLE imports `libkernel`) e valida??o de manifesto JSON. **11/11 testes aprovados, 72 checks no smoke**. |
| **S6** ? Pipeline gr?fico representativo | ?? Em Progresso | Renderiza??o nativa Metal (tri?ngulo colorido a 30 FPS) funcional no A17 Pro; pesquisa de tradu??o SPIR-V/MoltenVK. |
| **S7** ? Parser seguro do jogo | ? Planejado | An?lise est?tica de `eboot.bin` e `param.sfo` autorizados sem execu??o. |
| **S8** ? Boot e primeira instru??o guest | ? Planejado | Inicializa??o de m?dulos sob restri??es do iOS. |
| **S9** ? Jogabilidade e estabilidade | ? Planejado | Entrada, ?udio, persist?ncia de saves e termal/bateria. |

---

*Nota de integridade: A conclus?o dos marcos S0?S5 demonstra a maturidade da infraestrutura, do subsistema de mem?ria virtual, despacho din?mico e tradu??o offline arm64, com valida??o emp?rica em iPhone 15 Pro Max real.*
