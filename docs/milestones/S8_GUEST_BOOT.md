# Marco S8 ? Inicializa??o de M?dulos e Execu??o da Primeira Instru??o Guest

**Data:** 14/10/2026  
**Status:** ? **CONCLU?DO (LOADER DE SEGMENTOS ELF/PT_LOAD, PILHA GUEST E EXECU??O NATIVA INICIAL)**

---

## Objetivo do Marco (conforme ESCOPO)

> **S8**: inicializa??o de m?dulos e execu??o real da primeira instru??o do guest por mecanismo compat?vel com iOS; carregamento de segmentos de c?digo e dados, configura??o de registradores/pilha e handshake inicial; sem JIT ou viola??o de sandbox.

---

## Resumo T?cnico da Implementa??o

1. **Loader de Execut?vel Guest (`core/guest_loader.h/.cpp`):**
   - Mapeamento l?gico de todos os segmentos `PT_LOAD` do execut?vel (c?digo execut?vel `R-X` e dados/BSS `RW-`) diretamente para o espa?o de mem?ria gerenciado `GuestMemory`.
   - Preven??o de aloca??o de p?ginas com permiss?es simult?neas de escrita e execu??o (`RWX`), em estrita conformidade com o App Sandbox da Apple.
   - C?lculo e valida??o de `code_base`, `code_size`, `data_base`, `data_size` e `entry_point`.

2. **Configura??o de Contexto de CPU Guest (`GuestContext`):**
   - Aloca??o e inicializa??o de pilha de 64 KB para o guest (`stack_base = 0x70000000`), garantindo alinhamento de 16 bytes (`rsp & ~0xF`) exigido pela ABI System V AMD64 e ARM64.
   - Inicializa??o de registradores de prop?sito geral (`rax`, `rbx`, `rcx`, `rdx`, `rsi`, `rdi`, `r8-r15`) e flags de status (`rflags = 0x202`).
   - Apontamento de `rip` para o `entry_point` resolvido.

3. **Handshake de Entrada e Execu??o da Primeira Instru??o:**
   - Integra??o direta com a tabela do despachante indireto (`IndirectDispatcher`).
   - Execu??o controlada da rotina de entrada guest atrav?s do modelo de despacho compat?vel com arm64.
   - Disparo do handshake de kernel HLE (`sceKernelGetCurrentCpu`).
   - Valida??o do valor de retorno no registrador `rax` (`0xBB15`) e flags de status (`ZF`).

---

## Resultados dos Testes Automatizados no Host

```
Test project bloodborne-ios-lab/build-s5
      Start  1: memory ...........................   Passed    0.20 sec
      Start  2: threads ..........................   Passed    0.20 sec
      Start  3: alu ..............................   Passed    0.14 sec (300.432 or?culos x86-64)
      Start  4: integration ......................   Passed    0.09 sec (79 checks no smoke)
      Start  5: simd .............................   Passed    0.07 sec (F4 vetorial float/int/NaN)
      Start  6: dispatcher .......................   Passed    0.07 sec (F6 saltos indiretos e traps)
      Start  7: hle ..............................   Passed    0.08 sec (F6 chamadas externas / HLE)
      Start  8: concurrency ......................   Passed    0.09 sec (F5 atomics com 8 threads)
      Start  9: manifest .........................   Passed    0.10 sec (valida??o de manifesto JSON)
      Start 10: game_parser ......................   Passed    0.09 sec (S7 parser SFO e ELF)
      Start 11: guest_boot .......................   Passed    0.09 sec (S8 loader e primeira instru??o)
      Start 12: aot_rejection ....................   Passed    0.05 sec (fuzzing e muta??es AOT)
      Start 13: cli_integration ..................   Passed    0.12 sec (CLI de gera??o de c?digo)

100% dos testes passaram (13 de 13 aprovados)
```

### Diagn?stico do Smoke Runner:
```json
{
  "schema": 1,
  "build": "local-unversioned",
  "environment": "host",
  "checks": 79,
  "failures": 0,
  "game_execution": false
}
```
