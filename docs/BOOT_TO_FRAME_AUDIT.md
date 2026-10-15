# Auditoria do caminho até o primeiro quadro do jogo

Data: 15/10/2026. Estado observado no código do laboratório, não prova de execução de Bloodborne no iPhone.

## Cadeia de evidências

1. `host/ios/main.mm` localiza um `eboot.bin` ou `eboot.elf` e `dvdroot_ps4` nos Documentos do app. A presença desses caminhos não valida o formato, as dependências nem a integridade dos dados.
2. `GameBootCoordinator` monta arquivos, cria saves, carrega módulos, inicializa o runtime e avança passos de CPU. O contador de passos não demonstra título, cena ou jogabilidade.
3. `OrbisRuntime` encaminha alguns registros e flips de VideoOut ao `VideoOutManager`. O host agora anexa `GuestMemory` **antes** de inicializar módulos; registro inválido, submissão PM4 inválida e flip rejeitado não são mais contabilizados como sucesso nesses caminhos.
4. Se um flip aceito referenciar memória legível, o host copia pixels do guest para uma textura BGRA8 e tenta apresentá-la em tela cheia. O teste autoral `test_video_out_manager` verifica leitura de pixels inclusive com pitch maior que a largura; a UI só registra apresentação após conclusão do command buffer Metal.

## Bloqueios não resolvidos

- O caminho GNM ainda **não executa draws até produzir pixels do jogo**: o parser e `GnmContext` acumulam estados/chamadas, mas não há execução desses draws em render target Metal. O modelo 3D de demonstração não é vídeo do jogo.
- O registro HLE de VideoOut ainda assume quatro bytes por pixel, sem comprovar o formato e o layout da superfície (incluindo tiling). A cópia de bytes como BGRA8 **não prova** que a imagem é correta.
- A inicialização de módulos e os slices de CPU do app continuam síncronos no thread da UI. Adiar o auto-boot uma passagem do loop principal não elimina travamentos ou lentidão durante boot; desempenho e pico de memória devem ser medidos no iPhone.
- Não há validação nesta revisão de instalação, abertura, menus, controles, áudio/vídeo do jogo, fluxos de progresso nem estabilidade em iPhone físico. O workflow gera um IPA **sem assinatura e sem dados comerciais**.

## Próxima prova necessária

Uma fixture gráfica autoral deve produzir pixels na memória guest **por execução efetiva de comandos GPU**, passar por registro, submit, flip, leitura e apresentação, com diagnóstico por etapa. Só então investigar os formatos/targets e comandos reais usados pelo jogo, com dados mantidos localmente. Não marcar S9 como concluído com base em testes host ou compilação do IPA.
