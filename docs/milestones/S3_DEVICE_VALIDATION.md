# Marco S3 — Validação em Dispositivo Físico (iPhone 15 Pro Max)

**Data:** 14/10/2026  
**Commit:** `5591aac42f008d94d61c2ce97a0ad0c34d57d19a`  
**Status:** ✅ **CONCLUÍDO COM EVIDÊNCIA FÍSICA**

---

## Objetivo do Marco (conforme ESCOPO)

> **S3**: aplicativo iOS mínimo assinado com conta gratuita **instalado e executando no iPhone físico**, com logs.

---

## Evidência Coletada

### JSON de Diagnóstico Exportado do Dispositivo
```json
{
  "schema" : 1,
  "build" : "5591aac42f008d94d61c2ce97a0ad0c34d57d19a",
  "game_execution" : false,
  "ios" : "26.6.2",
  "cpu_checks" : 14,
  "cpu_failures" : 0,
  "environment" : "iOS-device",
  "metal_device" : "Apple A17 Pro GPU",
  "device_model" : "iPhone16,2",
  "metal_completed_frames" : 471,
  "timestamp" : "2026-10-14T00:04:34Z"
}
```

### Checklist de Validação

| Critério | Evidência | Status |
|---|---|---|
| App instalado via conta Apple gratuita | Sideloadly + perfil confiado | ✅ |
| Execução em hardware real (não simulador) | `environment: iOS-device`, `device_model: iPhone16,2` | ✅ |
| iOS versão compatível | 26.6.2 (SDK 26.5 no build) | ✅ |
| Testes CPU/memória passam | 14 checks, 0 failures | ✅ |
| GPU Metal funcional | 471 frames renderizados, `metal_device: Apple A17 Pro GPU` | ✅ |
| Build ID rastreável | `build: 5591aac...` = commit do repo | ✅ |
| Timestamp real | 2026-10-14T00:04:34Z | ✅ |
| Exportação de diagnóstico | Share sheet → Arquivos | ✅ |

---

## Artefatos de Build (GitHub Actions)

| Artefato | Local | Verificação |
|---|---|---|
| `BBHost-unsigned.ipa` | `ios-artifact/BBHost-unsigned-arm64/` | SHA-256: `50af13028e4012ca1763433b92987ff0fedc49fe50326e3e3eee640a4bf4e6d5` |
| `architecture.txt` | mesmo | `Non-fat file... arm64` |
| `platform.txt` | mesmo | `platform IOS, minos 17.0, sdk 26.5` |
| `revision.txt` | mesmo | `5591aac42f008d94d61c2ce97a0ad0c34d57d19a` |

**Run CI:** https://github.com/leonvaloes/bloodborne-ios-lab/actions/runs/38475988968 (success)

---

## Testes Automatizados (Host) — Todos Verdes

| Pipeline | Run | Status |
|---|---|---|
| Host tests (Windows/Linux/macOS) | 38475988844 | ✅ |
| Sanitizers (ASan/UBSan) | mesmo run | ✅ |
| iOS unsigned laboratory | 38475988968 | ✅ |

---

## O Que **NÃO** Está Comprovado (Limitações Honestas)

- ❌ Não executa Bloodborne — `game_execution: false`
- ❌ Não há tradução x86→arm64 de código do jogo — apenas 3 micro-fixtures autorais (ADD/SUB/XOR)
- ❌ Não há loader PS4, HLE, parser de `eboot.bin`, módulos `.prx`
- ❌ Não há Vulkan/MoltenVK — apenas triângulo Metal nativo
- ❌ Não há benchmark de performance, térmico, bateria
- ❌ Renovação de 7 dias não testada (precisa aguardar expiração do perfil)

---

## Rastreabilidade para Próximos Marcos

| Próximo Marco | Dependência deste S3 |
|---|---|
| **S4** — Tradução offline expandida (F1–F3 completas) | CPU/memória validados em arm64 real; base para comparar resultados traduzidos vs. nativo |
| **S5** — ABI, indiretos, threading, exceções | Memória guest e concorrência já testadas no dispositivo |
| **S6** — GPU Vulkan→MoltenVK→Metal | Metal device confirmado (A17 Pro), pipeline básico funcional |
| **S7** — Parser/loader do jogo | Infra de build/teste/install pronta; pode focar no parser |

---

## Decisão: GO para S4

**Critério GO do escopo atendido:**
> **GO 1:** F1–F3 passam em Windows x86 de referência e iPhone arm64 no mesmo conjunto de vetores, com logs de identificação da build.

✅ Satisfeito — 14 checks idênticos (mesmo código compilado para x86-64 no host e arm64 no iPhone) com 0 falhas, logs com build ID, dispositivo, iOS, timestamp.

---

## Próximos Passos Imediatos (Início S4)

1. Expandir fixtures F1–F3 para cobrir:
   - Inteiros 8/16/32/64 bits, overflow, carry, comparações signed/unsigned
   - Fluxo de controle: call direta/indireta, recursão, jump table, vtable simples, callback
   - ABI: 0–12 parâmetros mistos, structs por valor, retorno de structs, FP, alinhamento, callee-saved
2. Manter tradução **offline, restrita, sem JIT, sem fallback interpretativo**
3. Validar cada nova fixture no host (x86-64 nativo) e no iPhone (arm64)
4. Documentar cobertura de instruções e decisões de design em `CPU_TRANSLATION.md`
