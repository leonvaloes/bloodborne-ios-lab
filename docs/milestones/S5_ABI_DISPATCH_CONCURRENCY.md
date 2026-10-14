# Marco S5 ? ABI Representativa, Despacho Indireto, SIMD e Mem?ria Concorrente

**Data:** 14/10/2026  
**Status:** ? **CONCLU?DO NO HOST (11/11 TESTES APROVADOS + 72 CHECKS NO SMOKE)**

---

## Resultados do CI no GitHub Actions

| Pipeline | Run ID | Status | Detalhes |
|---|---|---|---|
| **Host tests** (Ubuntu, Windows MSVC, macOS Clang, Sanitizers) | [38501142468](https://github.com/leonvaloes/bloodborne-ios-lab/actions/runs/38501142468) | ? Sucesso (Verde) | Ubuntu, Windows, macOS, ASan/UBSan (1m09s) |
| **iOS unsigned laboratory** (Xcode arm64) | [38501142474](https://github.com/leonvaloes/bloodborne-ios-lab/actions/runs/38501142474) | ? Sucesso (Verde) | Build arm64 com SDK iOS 26.5 conclu?do em 47s |

### Artefatos de Build Produzidos (Run 38501142474)

| Arquivo | Verifica??o |
|---|---|
| `BBHost-unsigned.ipa` | SHA-256: `976ad2fc7b8ac8fc15a10b2c358752304611ffbc485651076420a7c65359fbf5` |
| `revision.txt` | `f8d4bc955d73650e273f15143ebaf06cf837ba12` |


## Objetivo do Marco (conforme ESCOPO)

> **S5**: demonstrar suporte a saltos indiretos, fun??es externas (HLE imports), ABI, mem?ria virtual e threading para uma carga representativa; sem JIT; atendendo aos crit?rios de GO 2 e GO 3.

---

## Componentes Desenvolvidos e Validados

### 1. F4 ? Vetoriza??o e Aritm?tica SIMD 128-bit (`core/simd.h`)
- Estrutura `Vec128` com alinhamento estrito de 16 bytes (`alignas(16)`), compat?vel com registradores SSE (`XMM`) e ARM NEON (`Q`).
- Opera??es vetoriais port?teis:
  - Adi??o e multiplica??o de 4 floats (`vec_add_f32`, `vec_mul_f32`).
  - Produto escalar vetorial de 4 elementos (`vec_dot_f32`).
  - Adi??o inteira vetorial de 4x 32 bits (`vec_add_i32`).
  - Conjun??o e disjun??o exclusiva bit a bit (`vec_and`, `vec_xor`).
  - Preserva??o estrita das regras IEEE 754 para valores `NaN` e `Infinity`.

### 2. F5 ? Mem?ria Concorrente e At?micos Guest (`core/guest_memory.h/.cpp`)
- Mapeamento e prote??o de p?ginas no modelo sandbox do iOS (sem aloca??o de p?ginas execut?veis RWX).
- Opera??es at?micas nativas com garantia de barreira e consist?ncia sequencial (equivalentes ao prefixo `LOCK` do x86):
  - `atomic_fetch_add_u64` / `atomic_fetch_add_u32` (`LOCK XADD`).
  - `atomic_compare_exchange_u64` / `atomic_compare_exchange_u32` (`LOCK CMPXCHG`).
- Teste de concorr?ncia massiva com 8 threads executando 40.000 opera??es at?micas concorrentes sobre a mesma regi?o de mem?ria com 0 falhas e integridade cont?bil estrita.

### 3. F6 ? Despacho de Saltos Indiretos (`core/dispatcher.h/.cpp`)
- Despacho din?mico de endere?os virtuais do guest PS4 (`guest_rip`) para fun??es nativas compiladas AOT:
  - Tabela de lookup com registro est?tico e din?mico (`register_target`).
  - Execu??o indireta com passagem de par?metros e flags (`dispatch`).
  - Tratamento de alvos n?o resolvidos / inv?lidos com lan?amento de exce??o segura e conten??o de falhas (sem crash do processo host).

### 4. F6 ? HLE Imports e Fun??es Externas (`core/hle.h/.cpp`)
- Subsistema de intercepta??o e adapta??o de chamadas do sistema PS4 (NIDs de `libkernel`):
  - `sceKernelGettimeofday` (NID 0x2001): escrita de timestamp na mem?ria guest.
  - `sceKernelGetCurrentCpu` (NID 0x2002): identifica??o de core l?gico.
  - `sceKernelAllocateDirectMemory` (NID 0x2003): aloca??o de mem?ria direta dentro do espa?o guest.
  - Registro extens?vel de novos manipuladores HLE com adapta??o de ABI.

### 5. Manifesto de Tradu??o Estruturado (`core/manifest.h/.cpp`)
- Gera??o e valida??o de manifesto JSON conforme a Se??o 7.4 do escopo:
  - Versionamento de esquema (`schema_version: 1`).
  - Identifica??o de arquiteturas (`x86_64` -> `arm64-apple-ios`).
  - Metadados de fun??es traduzidas (RVAs, tamanhos de bytes, s?mbolos de entrada, status de resolu??o de saltos).

---

## Resultados dos Testes Automatizados no Host

```
Test project bloodborne-ios-lab/build-s5
      Start  1: memory ...........................   Passed    0.20 sec
      Start  2: threads ..........................   Passed    0.20 sec
      Start  3: alu ..............................   Passed    0.14 sec (300.432 or?culos x86-64)
      Start  4: integration ......................   Passed    0.09 sec (72 checks no smoke)
      Start  5: simd .............................   Passed    0.08 sec (F4 vetorial float/int/NaN)
      Start  6: dispatcher .......................   Passed    0.08 sec (F6 saltos indiretos e traps)
      Start  7: hle ..............................   Passed    0.08 sec (F6 chamadas externas / HLE)
      Start  8: concurrency ......................   Passed    0.10 sec (F5 atomics LOCK com 8 threads)
      Start  9: manifest .........................   Passed    0.08 sec (valida??o de manifesto)
      Start 10: aot_rejection ....................   Passed    0.12 sec (fuzzing e muta??es AOT)
      Start 11: cli_integration ..................   Passed    0.13 sec (pipeline de gera??o CLI)

100% dos testes passaram (11/11 aprovados)
```

### Diagn?stico do Smoke Runner:
```json
{
  "schema": 1,
  "build": "local-unversioned",
  "environment": "host",
  "checks": 72,
  "failures": 0,
  "game_execution": false
}
```

---

## Transi??o para os Pr?ximos Marcos

Com o subsistema de tradu??o, mem?ria concorrente, saltos indiretos e HLE imports conclu?dos:
* **Marco S6:** Renderiza??o gr?fica representativa e adapta??o Vulkan/MoltenVK/Metal no A17 Pro GPU.
* **Marco S7:** Parser/loader de formato ELF / `eboot.bin` autorizado sem execu??o para inspe??o de cabe?alhos e se??es.
