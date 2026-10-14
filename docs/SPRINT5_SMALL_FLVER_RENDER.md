# Sprint 5 — malha do pequeno FLVER no host Metal

## Evidência recebida (iPhone físico)

Em 14/10/2026 14:26:48 UTC, build `bafe1c842ad63956e98f4fe20f1232844c83595e`, iPhone16,2/iOS 26.6.2: Foundation e VFS concordaram que somente o pequeno `m29_00_00_00_408010.flver.dcx` dentre os quatro arquivos esperados estava na raiz de Documents (1.024 bytes). `dcx_probe`: `ok`, 2.992 bytes descompactados, CRC32 2119878094/1284949640, `geometry_status=ok`, 23 vértices e 22 triângulos. `param.sfo`, `eboot.elf` e `teste-bblab.txt` não estavam nessa pasta nessa instalação. O app ainda desenhava Saw Cleaver sintética (8 vértices/12 triângulos); `game_execution=false`.

## Experimento seguinte

Na inicialização, o host tenta o pequeno DCX da raiz de Documents pelo probe limitado (1 MiB compactado, 8 MiB descompactado). Só seleciona a malha se a leitura, descompressão, parsing, contagens e índices dentro dos vértices forem válidos. Mantém o modelo de mapa já suportado como alternativa, depois a Saw Cleaver sintética. `model_source` identifica a origem da malha selecionada; `small_flver_render_status` registra `mesh_ready` ou o motivo de fallback. A alternativa de mapa agora exige malha não vazia, layout e índices válidos antes da seleção (`map_flver_selection_status`); falhas caem na malha sintética. `metal_mesh_status` distingue validação CPU de alocação de buffers Metal ou falha de alocação; `model_rendered=not_verified` evita confundir seleção com apresentação visual. `metal_flver_vertices` e `metal_flver_triangles` referem-se à malha selecionada para os buffers Metal, não à execução do jogo. `metal_completed_frames` registra conclusão de comandos da GPU, não prova visual isolada da imagem apresentada.

## Aceite e limites

Instalar o novo IPA sem desinstalar o app, conferir que o arquivo ainda está na raiz de Documentos de BB Lab, abrir o app, observar visualmente a malha e exportar diagnóstico. Esperado: `model_source=documents_small_flver_dcx`, `small_flver_render_status=mesh_ready`, 23 vértices/22 triângulos e quadros Metal crescentes. Caso contrário, anotar `documents_inventory`, `dcx_probe`, `model_source`, status de renderização e erro Metal (se houver). A exportação repete probes, mas a malha é escolhida **somente ao abrir o app**; depois de copiar o arquivo, feche e reabra o BB Lab. O teste Windows cobre parser/probe, não compila `main.mm` nem simula alocação Metal; compilar o alvo iOS no CI macOS e verificar diagnóstico no dispositivo são passos pendentes. Nenhuma textura/material/animação ou cena da engine foi validada; `game_execution=false`.

Windows: 25/25 testes CTest, smoke 118/0, fixture real opcional em `BB_TEST_DCX_PATH` passou com 23 vértices/22 triângulos. Esse dado privado permaneceu apenas no PC. Compilação iOS/CI e resultado físico do novo build devem ser registrados separadamente. Rollback: remover a seleção do pequeno DCX em `host/ios/main.mm`; o fallback anterior permanece.
