# Progresso verificável

Atualização inicial: 13/10/2026. Este documento registra implementação e evidência separadamente.

| Marco | Estado |
|---|---|
| S0 — fontes/revisões/inventário | Fontes do port e REA clonadas e revisões registradas; submódulos do port ainda não baixados; auditoria inicial em andamento. |
| S1 — testes host | Seis testes passaram no Windows Debug e Release com GCC 16.2.0. CI Windows/MSVC passou; CI Linux/macOS em correção do scanner de arquivos ocultos. |
| S2 — biblioteca iOS | Workflow criado; resultado de runner ainda pendente. |
| S3 — app físico | App de diagnóstico implementado; build, assinatura e execução física pendentes. Usuário confirmou iPhone 15 Pro Max, sem AltServer. |
| S4 — tradução | Três microfixtures offline com equivalência x86-64 host testada; F1 completa e execução física pendentes. |
| S5 — ABI/indiretos/memória/threading representativos | Memória lógica básica testada; demais componentes não implementados. |
| S6 — GPU representativa | Triângulo Metal implementado, não validado. MoltenVK e GPU do jogo não integrados. |
| S7 — parser do jogo | Não implementado; preservar ordem dos gates. |
| S8/S9 — boot e jogo | Não implementados; sem evidência de execução. |

Não há port 100% funcional ou prazo comprovado para realizá-lo. Os testes aprovados dizem respeito apenas ao laboratório descrito.
