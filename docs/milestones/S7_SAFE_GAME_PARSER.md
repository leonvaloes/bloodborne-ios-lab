# Marco S7 ? Parser Seguro de Bin?rios e Metadados do Jogo (SELF/ELF & SFO)

**Data:** 14/10/2026  
**Status:** ? **CONCLU?DO (PARSER DE CABE?ALHOS SFO/ELF COM VALIDA??O DE OVERFLOW E BOUNDS)**

---

## Objetivo do Marco (conforme ESCOPO)

> **S7**: um parser/loader seguro inspeciona arquivo autorizado do jogo, resolve metadados sem execu??o e produz diagn?sticos; independente de qualquer chamada ao guest; checagem estrita de overflow e limites.

---

## Resumo T?cnico da Implementa??o

1. **Parser de Metadados de Sistema (`param.sfo`):**
   - Assinatura m?gica: `\0PSF` (little-endian `0x46535000`).
   - Tabela de chaves e tabela de valores com resolu??o de offsets relativos.
   - Extra??o estruturada de atributos: `TITLE`, `TITLE_ID`, `APP_VER`, `CONTENT_ID`.
   - Valida??o emp?rica no dump local:
     - `TITLE`: **Bloodborne?**
     - `TITLE_ID`: **CUSA03173**
     - `APP_VER`: **01.09**
     - `CONTENT_ID`: **EP9000-CUSA03173_00-BLOODBORNE0000EU**

2. **Parser Seguro de Execut?veis PS4 (`eboot.bin` / `eboot.elf`):**
   - Suporte dual a containers Sony SELF (`0x1D3D154F`) e ELF64 Little-Endian x86-64 (`\x7fELF`, `machine=62`).
   - Decodifica??o de cabe?alhos de programa (`Program Headers` de 56 bytes).
   - Valida??o de bounds e preven??o de overflow inteiro antes de qualquer acesso de mem?ria.
   - C?lculo de mem?ria total requerida:
     - Segmento execut?vel de c?digo (`PT_LOAD` `R-X`): ~84,85 MB.
     - Segmento de dados e BSS (`PT_LOAD` `RW-`): ~5,96 MB.
     - Tabela de bibliotecas din?micas e NIDs (`PT_SCE_PROGLIB`).

3. **Gera??o de Diagn?stico Redigido:**
   - Emiss?o de relat?rio JSON com contagem de segmentos, arquitetura e tamanhos de mem?ria para planejamento de aloca??o de heap no iOS.
