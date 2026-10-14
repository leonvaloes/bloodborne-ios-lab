# Sprint 5 - pequeno asset FLVER.DCX

Proximo experimento no iPhone: ler um arquivo real de dados do jogo pelo VFS, descomprimir DCX/Deflate e confirmar a assinatura FLVER. Nao executa o jogo nem renderiza esse modelo.

Arquivo escolhido (somente copia local): `bloodborne_portPC/blodboorn 3171/Bloodborne(TM)/Image0/dvdroot_ps4/map/m29_00_00_00/m29_00_00_00_408010.flver.dcx`. O nome real da pasta contem o simbolo de marca registrada. Tem 1.024 bytes compactados e descomprime para 2.992 bytes em teste host. O arquivo original nao entra no Git nem no CI.

Instalacao do novo IPA: usar Sideloadly. Em Compartilhamento de Arquivos de BB Lab no iTunes/Dispositivos Apple, copiar **apenas** `m29_00_00_00_408010.flver.dcx` para a raiz de Documentos. Abrir BB Lab, tocar em **Verificar pequeno FLVER.DCX via VFS** e exportar diagnostico. A exportacao repete a leitura; o JSON contem `dcx_probe` com status, tamanho compactado/descompactado, CRC32 de ambos e horario. Esperado: `status = ok`, `valid = true`, 1.024 e 2.992 bytes. O CRC32 do arquivo compactado pode ser comparado ao original local.

Limites: tamanho compactado limitado a 1 MiB e tamanho declarado de descompressao a 8 MiB. O teste nao faz parsing completo de geometria, textura, cena, animacao ou execucao do `eboot`. `game_execution = false` e esperado. O aparelho ainda nao foi testado nesta etapa.
