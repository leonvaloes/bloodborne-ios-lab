# Sprint 5 — diagnóstico limitado de Documents

## Motivação e evidência

No diagnóstico do build `78b8001` no iPhone16,2/iOS 26.6.2, `boot_probe` encontrou `eboot.elf` (93.667.640 bytes) e parou depois de **duas** instruções x86-64; `documents_test`, `sfo_probe` e `dcx_probe` retornaram `not_found`. Testes anteriores, em builds diferentes, haviam lido individualmente os três arquivos. Não há evidência suficiente para determinar se foram removidos, se a instalação do app mudou o container ou se há divergência entre as APIs de arquivo. Não inferir que o jogo iniciou: `game_execution=false`.

## Alteração e próximo teste no aparelho

O relatório exportado inclui `documents_inventory`, uma inspeção somente de metadados dos quatro nomes conhecidos na **raiz** de Documents: `teste-bblab.txt`, `param.sfo`, `m29_00_00_00_408010.flver.dcx` e `eboot.elf`. Para cada nome, informa status e tamanho via Foundation e status via `VirtualFileSystem` montado em `/app0`. Também informa se Documents é um diretório e se a montagem foi aceita. Erros são apenas domínio/código; não exporta caminho absoluto, lista de outros arquivos nem conteúdo dos arquivos nesta seção. Os probes anteriores permanecem separados e repetidos na exportação; o teste de texto anterior ainda exporta seu conteúdo UTF-8 quando presente. Revise o JSON antes de compartilhá-lo.

Instalar o novo IPA preservando a instalação e os dados do app (não desinstalar antes). Em Compartilhamento de Arquivos do **BB Lab** no Dispositivos Apple/iTunes, verificar quais dos quatro nomes estão na raiz, sem pressupor persistência entre reinstalações/identificadores de assinatura. Exportar diagnóstico e comparar `documents_inventory.files` com `documents_test`, `sfo_probe`, `dcx_probe` e `boot_probe` no **mesmo** relatório/build. Se Foundation e VFS concordarem em `not_found`, conferir cópia, pasta de destino e container do app no dispositivo; se discordarem, investigar `vfs.stat`/resolução de caminho. Caso Foundation reporte `stat_error`, usar domínio/código para orientar investigação. Não transferir dados do jogo ao CI/GitHub.

Esperado: inventário coerente com arquivos realmente presentes no aparelho, sem alterar arquivos. Obtido: ainda não testado em iPhone nem compilado no runner macOS. Rollback: remover `checkDocumentsInventory` e a chamada na exportação. Não há mudança no loader nem na execução do jogo.

## Etapa seguinte: geometria do DCX

O mesmo `dcx_probe` agora distingue descompressão/assinatura (`status=ok`, `valid=true`) de parsing de malha (`geometry_status=ok`, `parse_failed`, `empty_geometry` ou `not_attempted`) e reporta vértices/triângulos somente após parsing. O fixture sintético com assinatura FLVER mas sem geometria não pode ser contado como modelo renderizável. Se o arquivo não estiver presente, o resultado continua `not_found` e `geometry_status=not_attempted`. Nenhum desses resultados significa cena renderizada ou jogo executado. O parsing atual pode aceitar um cabeçalho vazio; por isso `empty_geometry` é tratado separadamente. O app ainda desenha um modelo sintético por padrão.

## Verificação local

Windows, `tools/test-windows.ps1`: 25/25 testes CTest e smoke 118 verificações/0 falhas (`game_execution=false`). Teste opcional do DCX local (`BB_TEST_DCX_PATH` + `test_dcx_probe.exe`) saiu com código 0; não registra nem publica o arquivo ou dados extraídos. O host Windows não compila `host/ios/main.mm`; a compilação iOS e a verificação de inventário/geometria no iPhone ficam pendentes.
