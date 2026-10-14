# Sprint 5 - pequeno asset FLVER.DCX

Proximo experimento no iPhone: ler um arquivo real de dados do jogo pelo VFS, descomprimir DCX/Deflate e confirmar a assinatura FLVER. Nao executa o jogo nem renderiza esse modelo.

Arquivo escolhido (somente copia local): `bloodborne_portPC/blodboorn 3171/Bloodborne(TM)/Image0/dvdroot_ps4/map/m29_00_00_00/m29_00_00_00_408010.flver.dcx`. O nome real da pasta contem o simbolo de marca registrada. Tem 1.024 bytes compactados e descomprime para 2.992 bytes em teste host. O arquivo original nao entra no Git nem no CI.

Instalacao do novo IPA: usar Sideloadly. Em Compartilhamento de Arquivos de BB Lab no iTunes/Dispositivos Apple, copiar **apenas** `m29_00_00_00_408010.flver.dcx` para a raiz de Documentos. Abrir BB Lab, tocar em **Verificar pequeno FLVER.DCX via VFS** e exportar diagnostico. A exportacao repete a leitura; o JSON contem `dcx_probe` com status, tamanho compactado/descompactado, CRC32 de ambos e horario. Esperado: `status = ok`, `valid = true`, 1.024 e 2.992 bytes. O CRC32 do arquivo compactado pode ser comparado ao original local.

Limites: tamanho compactado limitado a 1 MiB e tamanho declarado de descompressao a 8 MiB. O teste nao faz parsing completo de geometria, textura, cena, animacao ou execucao do `eboot`. `game_execution = false` e esperado. A leitura e a descompressao foram posteriormente validadas no aparelho; parsing integral e renderizacao deste modelo continuam pendentes.

## Evidencia no iPhone (14/10/2026, 12:46:02 UTC)

Diagnostico fornecido pelo usuario, build `061e6be649b48708fb51d260fdfb5cb9105d68ee`, iPhone 15 Pro Max (`iPhone16,2`, iOS 26.6.2): `dcx_probe.status = ok`, `valid = true`, `size_bytes = 1024`, `crc32 = 2119878094`, `decoded_bytes = 2992`, `decoded_crc32 = 1284949640`. Os quatro valores conferem com o arquivo local; a carga descompactada comeca com `FLVER\0`. O app registrou 118 verificacoes CPU com 0 falhas e `game_execution = false`.

`documents_test.status = not_found` e `sfo_probe.status = not_found` indicam que esses arquivos nao estavam presentes na instalacao testada; nenhum deles e pre-requisito para este probe. A renderizacao reportada (8 vertices/12 triangulos) e a cena sintetica do host, nao evidencia de renderizacao desse arquivo FLVER no iPhone.
