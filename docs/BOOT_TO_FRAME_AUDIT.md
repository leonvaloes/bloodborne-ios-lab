# Auditoria do caminho até o primeiro quadro do jogo

Data: 15/10/2026. Estado observado no código do laboratório, não prova de execução de Bloodborne no iPhone.

## Cadeia de evidências

1. `host/ios/main.mm` localiza um `eboot.bin` ou `eboot.elf` e `dvdroot_ps4` nos Documentos do app. A presença desses caminhos não valida o formato, as dependências nem a integridade dos dados.
2. `GameBootCoordinator` monta arquivos, cria saves, carrega módulos, inicializa o runtime e avança passos de CPU. O contador de passos não demonstra título, cena ou jogabilidade.
3. `OrbisRuntime` encaminha alguns registros e flips de VideoOut ao `VideoOutManager`. O host agora anexa `GuestMemory` **antes** de inicializar módulos; registro inválido, submissão PM4 inválida e flip rejeitado não são mais contabilizados como sucesso nesses caminhos.
4. Se um flip aceito referenciar memória legível, o host copia pixels do guest para uma textura BGRA8 e tenta apresentá-la em tela cheia. O teste autoral `test_video_out_manager` verifica leitura de pixels inclusive com pitch maior que a largura; a UI só registra apresentação após conclusão do command buffer Metal.

## Bloqueios não resolvidos

- O caminho GNM ainda **não executa draws até produzir pixels do jogo**: o parser e `GnmContext` acumulam estados/chamadas, mas não há execução desses draws em render target Metal. O modelo 3D de demonstração não é vídeo do jogo.
- O registro HLE de VideoOut ainda assume quatro bytes por pixel, sem comprovar o formato e o layout da superfície (incluindo tiling). O host iOS só apresenta a cópia para buffers declarados `0x80000000`, quatro bytes por pixel e `tiling_mode=1`; isso evita exibir buffers marcados tiled como se fossem lineares, mas **não prova** correção de imagens do jogo.
- A inicialização de módulos e os slices de CPU do app continuam síncronos no thread da UI. Adiar o auto-boot uma passagem do loop principal não elimina travamentos ou lentidão durante boot; desempenho e pico de memória devem ser medidos no iPhone.
- Não há validação nesta revisão de instalação, abertura, menus, controles, áudio/vídeo do jogo, fluxos de progresso nem estabilidade em iPhone físico. O workflow gera um IPA **sem assinatura e sem dados comerciais**.

## Próxima prova necessária

O teste host autoral `test_pm4_to_frame` percorre PM4 → estado GNM → rasterizador **CPU** → bytes do RT guest → HLE VideoOut flip → leitura do frame. O runtime agora expõe status/counter de falhas do rasterizador, sem transformar uma falha do fallback CPU em confirmação de desenho; o código HLE ainda retorna sucesso de submissão após falha do rasterizador e deve ser interpretado com essa ressalva. O clear inicial é feito antes do PM4, não por comando GPU. Ainda falta um encoder Metal de draws PM4 e prova de apresentação no iPhone pela cadeia guest (não confundir com a cena sintética já validada). Só então investigar formatos/targets e comandos usados pelo jogo, mantendo os dados localmente. Não marcar S9 concluído com base em testes host ou compilação do IPA.
