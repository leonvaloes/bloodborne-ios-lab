# Marco Sprint 5: leitura de Documents no iPhone

Data do experimento: 14/10/2026 (diagnostico `2026-10-14T12:03:22Z`).

## Ambiente e versao

- Dispositivo informado pelo diagnostico: iPhone 15 Pro Max (`iPhone16,2`), iOS `26.6.2`, GPU Apple A17 Pro.
- Build: `9fc9c9852734e0157c9553c8802a786e6d05c388` (IPA ARM64 compilado no GitHub Actions).
- Transferencia: usuario instalou o BB Lab via Sideloadly e copiou `teste-bblab.txt` usando Compartilhamento de Arquivos do iTunes.

## Procedimento e criterio

Abrir BB Lab, usar o teste de arquivo em Documents e exportar o JSON. Esperado: o app encontra um arquivo regular, de ate 64 KiB, nao vazio e em UTF-8; o diagnostico registra `documents_test.status = "ok"`, `valid = true` e o conteudo efetivamente lido. Nao havia um texto esperado fornecido antes da transferencia; a validacao e estrutural e registra o texto para comparacao pelo usuario.

## Obtido no aparelho

O JSON exportado e fornecido pelo usuario mostrou `documents_test.status = "ok"`, `valid = true`, `size_bytes = 42` e `content_utf8 = "Teste de transferencia BB Lab - Sprint 5\r\n"`. Essa string possui 40 bytes ASCII mais CRLF (2 bytes), consistente com o tamanho registrado. O diagnostico reportou `cpu_checks = 118`, `cpu_failures = 0`, `metal_completed_frames = 2250` e `average_fps = 29.993747548376689` no host sintetico.

## Limites

A evidencia confirma a leitura desse arquivo no sandbox `Documents` do aplicativo nesse build e o registro exportavel do resultado. Nao comprova leitura de pacotes BHD5/BDT reais, montagem do jogo no VFS, boot do executavel, renderizacao do jogo ou jogabilidade: `game_execution = false`. O modelo de 8 vertices/12 triangulos reportado e a cena sintetica do laboratorio, nao uma cena de Bloodborne validada no aparelho. Nao foram coletados hash do arquivo original, log bruto ou teste com arquivo de jogo.
