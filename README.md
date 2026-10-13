# Bloodborne iOS Lab

Laboratório autoral de pesquisa para iPhone 15 Pro Max, desenvolvido no Windows. **Ainda não executa Bloodborne.** Não inclui arquivos do jogo nem fontes copiadas do runtime upstream.

## Implementado

- Memória guest lógica com orçamento, proteção de leitura/escrita, bounds checking e sincronização por mutex.
- Tradutor offline **restrito a três fixtures de sete bytes**: `MOV RAX,RDI; ADD/SUB/XOR RAX,RSI; RET`. Gera C++ que é compilado junto ao app. Não interpreta arquivos do jogo, não implementa CPU geral e não utiliza JIT.
- Comparação de resultados/flags com x86-64 nativo em hosts GCC/Clang x86-64; vetores fixos em outros hosts.
- App iOS de diagnóstico com testes CPU/memória, triângulo Metal e exportação de JSON. A compilação/execução no aparelho tem status independente dos testes host.
- CI para Windows/Linux/macOS, sanitizers e empacotamento iOS arm64 sem assinatura.

## Testes locais Windows

Com a toolchain portátil em `../.tools/w64devkit/bin`, execute:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test-windows.ps1 -Configuration Debug
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test-windows.ps1 -Configuration Release
```

`-ExecutionPolicy Bypass` se aplica apenas ao processo que executa o script revisável, sem mudar a política persistente do Windows. Alternativamente execute diretamente CMake/CTest com um compilador configurado:

```text
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

Os testes usam somente código e dados autorais. Não dependem de GPU, jogo, REA ou rede. `bb-smoke` retorna JSON e exit code diferente de zero quando falha.

## Limites atuais

Não há loader PS4, HLE integrada, chamadas indiretas, ABI completa, SIMD/AVX, tradutor geral ou backend MoltenVK. O triângulo usa Metal diretamente e não demonstra compatibilidade com o renderer do jogo. A compilação das fixtures em arm64 não satisfaz sozinha S4: os resultados precisam ser verificados no dispositivo, e F1 completa inclui mais larguras/instruções.

Consulte [PROGRESS.md](docs/PROGRESS.md), [TESTING.md](docs/TESTING.md), [UPSTREAM.md](docs/UPSTREAM.md) e [DEVICE_TEST.md](docs/DEVICE_TEST.md). A arquitetura de referência é o `ESCOPO_BLOODBORNE_IOS_WINDOWS_REA.md` no workspace pai.
