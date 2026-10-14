# Estratégia de testes

## Testes automatizados existentes

| Teste | Evidência produzida |
|---|---|
| `memory` | Rejeição de overflow, regiões sobrepostas, orçamento excedido, ponteiro nulo, acesso entre regiões, proteção e unmap; 20.000 transferências pseudoaleatórias contra modelo de referência. |
| `threads` | 8 threads, 16.000 ciclos map/store/load/unmap em regiões distintas; valida integridade e contabilidade final. Não prova atomics/scheduler do guest. |
| `alu` | Vetores extremos; em x86-64 GCC/Clang, 300.432 comparações de valor e flags contra ADD/SUB/XOR executados nativamente. AF de XOR é indefinida e excluída. |
| `aot_rejection` | Entrada textual malformada, truncamentos, bytes extras, 1.792 mutações e 10.000 entradas pseudoaleatórias rejeitadas. |
| `integration` | Fixtures traduzidas e compiladas ligadas ao núcleo de memória; 14 verificações do runner compartilhado com iOS. |
| `cli_integration` | Processo tradutor, erro sem sobrescrever saída, geração válida, processo smoke, JSON e exit code. |

`check-source.ps1` verifica os arquivos rastreados antes do envio. Extensões permitidas e detecção limitada de chaves/tokens são uma proteção adicional, não garantia de ausência de conteúdo sensível.

## Ambientes

- Windows local: GCC portátil; Debug e Release são execuções independentes.
- CI Windows: compilador MSVC padrão, portanto usa vetores fixos em `alu`.
- CI Ubuntu: GCC x86-64 com oráculo nativo; job Clang separado com AddressSanitizer/UndefinedBehaviorSanitizer.
- CI macOS: compilador Apple Clang; se arm64, só vetores fixos, não oráculo x86.
- iOS: o mesmo smoke de 14 verificações, compilado antecipadamente para arm64; Metal e exportação manual. Nenhum resultado de aparelho deve ser inferido dos jobs host.

## Consolidação local em 14/10/2026

No checkout de trabalho desta data, `tools/test-windows.ps1 -Configuration Release` e `-Configuration Debug` compilaram e executaram **50/50 testes CTest** com 100% de sucesso; o smoke host registrou 118 verificações, nenhuma falha e `game_execution=false`.
As correções recentes consolidadas abrangem:
- **`x86_interp`**: instruções `BT`, `BTS`, `BTR` e `BTC` com operando em memória (0x0F A3/AB/B3/BB) com cálculo de deslocamento de bits com sinal e modificação segura de bytes; correção de `LEA` sem aplicar bases de segmento FS/GS; decodificação de `66 68` (PUSH imm16) empilhando 2 bytes; shifts com count zero sem escrita de memória.
- **`OrbisRuntime` & stdio HLE**: tabela de descritores opacos de arquivos com handles isolados e fechamento no destrutor; funções stdio (`fopen`, `fclose`, `fread`, `fwrite`, `fseek`, `ftell`, `fgets`, `fgetc`, `fputc`, `fputs`, `remove`, `rename`, `mkdir`, `rmdir`) operando estritamente via buffers intermediários com verificação de limites em `GuestMemory` e bloqueio de path traversal (`..`); conformidade ABI SysV FP lendo registradores XMM incondicionalmente mesmo para escalares zero (+0.0/-0.0).
- **`libSceVideoOut` HLE**: implementação dos handlers `sceVideoOutOpen`, `sceVideoOutClose`, `sceVideoOutSetFlipRate`, `sceVideoOutAddFlipEvent`, `sceVideoOutSetBufferAttribute`, `sceVideoOutRegisterBuffers`, `sceVideoOutSubmitFlip` e `sceVideoOutGetFlipStatus`, interligados com `VideoOutManager` e `GameBootCoordinator`.
- **`GuestScheduler`**: alinhamento estrito de stack SysV na entrada da thread (`rsp % 16 == 8`), expiração e remoção confiável de waiters de mutex expirados/cancelados da fila de sincronização.
- **`VideoOut` & `VideoOutManager`**: teto rígido de 64 MiB por buffer de vídeo registrado, prevenção de overflow em pitch * height e validação integral com `GuestMemory`.

Estes resultados são **Windows host**, não build iOS desta revisão, instalação no iPhone, apresentação Metal de pixels do guest nem execução do motor no dispositivo.

O alvo iOS `BBHost` em `host/ios/` continua independente do protótipo diagnóstico `bb-ios-lab` em `ios/`. O workflow `.github/workflows/ios.yml` pretende compilar e empacotar **BBHost sem assinatura**. O protótipo não carrega `Image0` nem executa código guest automaticamente; sua compilação iOS ainda precisa de verificação no runner. Relatórios anteriores de aparelho referem-se às revisões especificadas em `docs/milestones/` e não validam as mudanças atuais.

## Limitações

AOT aqui significa decodificar um subconjunto fechado de bytes autorais e gerar fonte estática, depois compilá-la para a arquitetura alvo. As fixtures foram escritas manualmente como instruções, não extraídas do executável comercial. Não há descoberta de funções, relocations, jumps ou tradução de binários arbitrários. Não há benchmark de performance nem alegação de jogabilidade.

Os testes de memória não implementam instruções atômicas do guest. Uma operação é serializada pelo mutex, mas um par load/store separado não é read-modify-write atômico. Transferências de tamanho zero e que cruzam regiões são rejeitadas por contrato.
