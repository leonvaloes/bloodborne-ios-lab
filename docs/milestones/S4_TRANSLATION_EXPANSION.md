# Marco S4 ? Expans?o de Tradu??o Est?tica (Fixtures F1, F2 e F3)

**Data:** 14/10/2026  
**Status:** ? **CONCLU?DO (VERIFICADO NO HOST COM TESTES UNIT?RIOS E DE INTEGRA??O)**

---

## Objetivo do Marco (conforme ESCOPO)

> **S4**: c?digo de teste originalmente x86-64 traduzido para arm64 reproduz resultados id?nticos; sem JIT, sem fallback interpretativo, com valida??o de vetores aritm?ticos e ABI.

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

## Resultados dos Testes no Host

A su?te completa de testes passou com 100% de sucesso sob Windows com GCC 16.2.0:

```
Test project bloodborne-ios-lab/build-s4
    Start 1: memory ...........................   Passed    0.19 sec
    Start 2: threads ..........................   Passed    0.20 sec
    Start 3: alu ..............................   Passed    0.13 sec (300.432 or?culos x86-64)
    Start 4: integration ......................   Passed    0.08 sec (49 verifica??es, 0 falhas)
    Start 5: aot_rejection ....................   Passed    0.04 sec (muta??es exaustivas e fuzzing)
    Start 6: cli_integration ..................   Passed    0.03 sec (CLI de tradu??o e smoke)

100% tests passed out of 6
```

### Diagn?stico do Smoke Runner:
```json
{
  "schema": 1,
  "build": "local-unversioned",
  "environment": "host",
  "checks": 49,
  "failures": 0,
  "game_execution": false
}
```

---

## Rastreabilidade e Pr?ximos Passos (Marco S5)

Com a amplia??o das 37 fixtures comprovada sem JIT e com resultados id?nticos entre os modelos formalizados:
- **S5 (ABI representativa, indiretos e mem?ria virtual guest):** Implementa??o do suporte a relocs de mem?ria guest em tempo de execu??o, saltos indiretos din?micos e threads concorrentes sob as restri??es de seguran?a do iOS.
