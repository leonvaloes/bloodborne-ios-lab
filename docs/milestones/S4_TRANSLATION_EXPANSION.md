# Marco S4 ? Expans?o de Tradu??o Est?tica (Fixtures F1, F2 e F3)

**Data:** 14/10/2026  
**Commit:** `52d3441267302274dad7c23c0cdb2c547b6a0b4e`  
**Status:** ? **CONCLU?DO COM EVID?NCIA F?SICA NO IPHONE 15 PRO MAX**

---

## Objetivo do Marco (conforme ESCOPO)

> **S4**: c?digo de teste originalmente x86-64 traduzido para arm64 reproduz resultados id?nticos; sem JIT, sem fallback interpretativo, com valida??o de vetores aritm?ticos e ABI.

---

## Evid?ncia Coletada no Dispositivo F?sico (iPhone 15 Pro Max)

```json
{
  "schema" : 1,
  "build" : "52d3441267302274dad7c23c0cdb2c547b6a0b4e",
  "game_execution" : false,
  "ios" : "26.6.2",
  "cpu_checks" : 49,
  "cpu_failures" : 0,
  "environment" : "iOS-device",
  "metal_device" : "Apple A17 Pro GPU",
  "device_model" : "iPhone16,2",
  "metal_completed_frames" : 189,
  "timestamp" : "2026-10-14T02:03:00Z"
}
```

### Checklist de Valida??o do Marco S4

| Crit?rio | Evid?ncia | Status |
|---|---|---|
| Instala??o f?sica via sideload | Sideloadly no iPhone 15 Pro Max | ? |
| Execu??o em hardware real | `environment: iOS-device`, `device_model: iPhone16,2` | ? |
| CPU Checks expandidos (F1?F3) | **49 checks, 0 falhas** | ? |
| GPU Metal ativa | 189 quadros Metal renderizados nativamente | ? |
| Equival?ncia Host vs Dispositivo | 49 checks id?nticos no Windows GCC e no Apple A17 Pro arm64 | ? |
| Build rastre?vel | `52d3441...` correspondente ao commit no GitHub | ? |
| Or?culos x86-64 nativos | 300.432 compara??es passando no host x86-64 | ? |
| Aus?ncia de JIT / P?ginas RWX | 100% compila??o AOT est?tica compat?vel com App Sandbox | ? |

---

## Resumo T?cnico da Implementa??o

Nesta etapa, o tradutor AOT est?tico (`bb-aot`) e o pipeline de execu??o e testes foram expandidos de 3 microfixtures originais para um conjunto abrangente de 37 fixtures autorais, divididas em 3 fam?lias principais:

### 1. Fam?lia F1 ? Aritm?tica, L?gica e Flags de Status
- **Larguras de Inteiros:** 8, 16, 32 e 64 bits para `ADD` e `SUB`.
- **Aritm?tica com Carry/Borrow:** Implementa??es de `ADC` (Add with Carry) e `SBB` (Subtract with Borrow) suportando encadeamento multiprecis?o.
- **Opera??es L?gicas Bitwise:** `AND`, `OR`, `XOR` em 8 e 64 bits.
- **Deslocamentos (Shifts):** `SHL` (deslocamento l?gico ? esquerda) e `SHR` (deslocamento l?gico ? direita) com c?lculo exato de flags.
- **Compara??es e Testes:** `CMP` (subtra??o sem escrita de destino) e `TEST` (conjun??o sem escrita de destino) com flags `CF`, `ZF`, `SF`, `OF`, `AF` e `PF` estritamente calculadas.
- **Valida??o Cruzada:** 300.432 testes comparativos contra instru??es nativas x86-64 executadas na CPU host aprovados em 100%.

### 2. Fam?lia F2 ? Fluxo de Controle
- **Chamada Direta:** Chamadas de fun??o est?ticas (`CALL rel32`).
- **Chamada Indireta:** Chamadas via ponteiro/registrador (`CALL reg/mem`).
- **Recurs?o:** Teste de profundidade de pilha e conven??o de chamada recursiva (fatorial de 5 = 120).
- **Jump Table:** Sele??o est?tica indexada (simula??o de `switch/case` otimizado por tabela de saltos).
- **Vtable Dispatch:** Despacho polim?rfico C++ simulado por ponteiro de tabela virtual.
- **Callbacks:** Passagem de ponteiros de fun??o de volta para c?digo de ordem superior.

### 3. Fam?lia F3 ? Conformidade com ABI (System V AMD64 / ARM64)
- **Zero e Um Par?metro:** Conven??o de retorno imediato e passagem ?nica em registrador.
- **Seis Par?metros:** Uso integral dos registradores padr?o de argumentos inteiros (`RDI`, `RSI`, `RDX`, `RCX`, `R8`, `R9`).
- **Doze Par?metros:** Passagem mista via registradores + transbordamento na pilha (*stack arguments*).
- **Estruturas por Valor (Struct by Value):** Passagem de agregados via registradores/ponteiro impl?cito.
- **Retorno de Estruturas (Struct Return):** Aloca??o oculta pelo chamador com passagem de ponteiro no primeiro argumento (`sret`).
- **Ponto Flutuante (FP Args):** Par?metros de precis?o dupla em registradores escalares (`XMM0`, `XMM1` / `D0`, `D1`).
- **Alinhamento de Pilha:** Verifica??o de preserva??o do alinhamento obrigat?rio de 16 bytes (`SP % 16 == 0`).
- **Registradores Callee-Saved:** Preserva??o estrita dos registradores preservados pelo chamado (`RBX`, `RBP`, `R12-R15` / `X19-X28`).

---

## Resultados dos Testes no Host (Windows GCC 16.2.0)

A su?te completa de testes passou com 100% de sucesso sob Windows com GCC 16.2.0:

```
Test project bloodborne-ios-lab/build-s4
    Start 1: memory ...........................   Passed    0.19 sec
    Start 2: threads ..........................   Passed    0.20 sec
    Start 3: alu ..............................   Passed    0.13 sec (300.432 or?culos x86-64)
    Start 4: integration ......................   Passed    0.08 sec (49 verifica??es, 0 falhas)
    Start 5: aot_rejection ....................   Passed    0.04 sec (muta??es exaustivas e fuzzing)
    Start 6: cli_integration ..................   Passed    0.03 sec (CLI de tradu??o e smoke)

100% dos testes passaram (6/6 aprovados)
```

---

## Resultados do CI no GitHub Actions

| Pipeline | Run ID | Status | Detalhes |
|---|---|---|---|
| **Host tests** (Ubuntu, Windows MSVC, macOS Clang, Sanitizers) | [38497368343](https://github.com/leonvaloes/bloodborne-ios-lab/actions/runs/38497368343) | ? Sucesso (Verde) | Ubuntu (16s), Windows (50s), macOS (13s), ASan/UBSan (19s) |
| **iOS unsigned laboratory** (Xcode arm64) | [38497368323](https://github.com/leonvaloes/bloodborne-ios-lab/actions/runs/38497368323) | ? Sucesso (Verde) | Build arm64 com SDK iOS 26.5, packaging do IPA conclu?do em 39s |

### Artefatos de Build Produzidos (Run 38497368323)

| Arquivo | Verifica??o |
|---|---|
| `BBHost-unsigned.ipa` | SHA-256: `a8c3140fa852808811a98afc8ef16d4d71f1c9e7ecbbfc8736478f088c2ea398` |
| `architecture.txt` | `Non-fat file... arm64` |
| `platform.txt` | `platform IOS, minos 17.0, sdk 26.5` |
| `revision.txt` | `52d3441267302274dad7c23c0cdb2c547b6a0b4e` |

---

## Decis?o: GATE CPU GO 1 ATENDIDO COM SUCESSO

> **Crit?rio GO 1 do Escopo:** F1?F3 passam em Windows x86 de refer?ncia e iPhone arm64 no mesmo conjunto de vetores, com logs de identifica??o da build.

? **Atingido com louvor.** 49 checks executados e validados no iPhone 15 Pro Max.

---

## Rastreabilidade e Transi??o para o Marco S5

O projeto avan?a formalmente para o **Marco S5**:
- F4 (opera??es vetoriais SIMD / NEON)
- F5 (mem?ria concorrente, atomics x86 `LOCK CMPXCHG`, multithreading guest)
- F6 (Thunks de chamadas externas / HLE imports)
- Despacho indireto din?mico e manifesto de tradu??o.
