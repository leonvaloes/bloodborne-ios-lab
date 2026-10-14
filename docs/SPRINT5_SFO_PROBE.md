# Sprint 5 - experimento de leitura de metadados SFO

Objetivo: validar no iPhone uma leitura pequena de dado real do jogo pelo VFS, sem executar o jogo.

## Ambiente e procedimento

- Arquivo local de referencia: `bloodborne_portPC/blodboorn 3171/Bloodborne(TM)/Sc0/param.sfo` (o nome real da pasta inclui o simbolo de marca registrada). Tem 1.760 bytes no workspace observado em 14/10/2026.
- O arquivo original permanece no PC. O usuario copia somente `param.sfo` para a raiz de **Documentos de BB Lab** no Compartilhamento de Arquivos do iTunes/Dispositivos Apple.
- No novo BB Lab, tocar em **Verificar param.sfo via VFS** e depois **Exportar diagnostico de estabilidade**. O botao de exportacao repete o teste e inclui `sfo_probe` no JSON.
- O app monta o diretorio `Documents` em `/app0` somente leitura, verifica o tamanho (20 bytes a 1 MiB), le `/app0/param.sfo` pelo `VirtualFileSystem`, calcula CRC32 e usa `SafeGameParser::parse_sfo`. Exige `TITLE_ID` nao vazio.

## Esperado e evidencias

Esperado: `sfo_probe.status = "ok"`, `valid = true`, `size_bytes`, `crc32`, `title_id`, `title` e `app_ver`. Se ausente/invalido, o JSON registra um status especifico. O resultado obtido no iPhone ainda depende do diagnostico exportado pelo usuario; nao presumir sucesso no aparelho pela compilacao.

Teste host: `test_game_asset_probe` cobre ausente, sintese valida, cabecalho invalido e limite de tamanho. Com `BB_TEST_SFO_PATH` apontando ao arquivo local, valida tambem uma copia temporaria do SFO real, sem adiciona-lo ao Git. CI testa fixtures sinteticas; nao envia dados do jogo.

## Limites

Este teste valida somente montagem do diretorio, leitura de um arquivo de metadados e parsing SFO. Nao demonstra ingestao de BHD5/BDT, descompressao DCX, execucao do `eboot`, graficos do jogo ou jogabilidade. `game_execution` permanece `false`.
