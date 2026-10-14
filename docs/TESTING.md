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

No checkout de trabalho desta data, `tools/test-windows.ps1 -Configuration Release` compilou e executou **50/50 testes CTest**; o smoke host registrou 118 verificações, nenhuma falha e `game_execution=false`. Os novos testes `host_bridge` cobrem principalmente estados sem inicialização; `video_out_manager` usa pixels autorais para verificar leitura completa, limites de tamanho e ausência de frames fabricados. Os testes AVX adicionais são sintéticos. Estes resultados são **Windows host**, não build iOS desta revisão, instalação no iPhone, apresentação Metal de pixels do guest nem execução do motor no dispositivo.

O alvo iOS `BBHost` em `host/ios/` continua independente do protótipo diagnóstico `bb-ios-lab` em `ios/`. O workflow `.github/workflows/ios.yml` pretende compilar e empacotar **BBHost sem assinatura**. O protótipo não carrega `Image0` nem executa código guest automaticamente; sua compilação iOS ainda precisa de verificação no runner. Relatórios anteriores de aparelho referem-se às revisões especificadas em `docs/milestones/` e não validam as mudanças atuais.

## Limitações

AOT aqui significa decodificar um subconjunto fechado de bytes autorais e gerar fonte estática, depois compilá-la para a arquitetura alvo. As fixtures foram escritas manualmente como instruções, não extraídas do executável comercial. Não há descoberta de funções, relocations, jumps ou tradução de binários arbitrários. Não há benchmark de performance nem alegação de jogabilidade.

Os testes de memória não implementam instruções atômicas do guest. Uma operação é serializada pelo mutex, mas um par load/store separado não é read-modify-write atômico. Transferências de tamanho zero e que cruzam regiões são rejeitadas por contrato.
