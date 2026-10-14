# Sideload no Windows — procedimento experimental para o BB Lab

**Estado deste guia (14/10/2026):** há registros de builds anteriores do laboratório sintético compilados no CI e testados no iPhone em `docs/milestones/` e `docs/SPRINT5_DOCUMENTS_DEVICE.md`. **Isso não valida a revisão atual nem o protótipo `bb-ios-lab`**: para cada novo artefato, confirme build, assinatura, instalação e abertura separadamente. O workflow `.github/workflows/ios.yml` empacota `BBHost` sem assinatura. Os registros anteriores mantêm `game_execution=false`; não há Bloodborne jogável. Comece com fixtures autorais, **sem Image0, dumps ou assets do jogo**.

Este roteiro só se aplica **se** o workflow da revisão escolhida produzir um artefato válido e uma ferramenta de assinatura pessoal aceitar o pacote. Compilar, baixar, assinar, instalar e abrir são etapas distintas; registre a evidência de cada uma antes de avançar. Sem Mac próprio, o runner macOS pode compilar o alvo iOS; a instalação **desta revisão** pelo Windows ainda precisa ser verificada no aparelho.

## 1. Artefato esperado no CI

A referência deste guia é **`.github/workflows/ios.yml`**, workflow **iOS unsigned laboratory** (não `.github/workflows/ci.yml`). Se o job `ios` terminar com sucesso, a seção **Artifacts** da execução deve oferecer **`BBHost-unsigned-arm64`**; o ZIP contém `BBHost-unsigned.ipa`, `SHA256SUMS.txt`, `architecture.txt`, `platform.txt` e `revision.txt`. O workflow pretende empacotar `Payload/BBHost.app`, verificar a arquitetura `arm64` e manter o artefato por sete dias. Esses nomes são a configuração do YAML, **não comprovantes de que o build ocorreu ou o IPA é instalável**. O deployment target configurado é iOS 17.0, não uma garantia de compatibilidade com uma versão ou aparelho específico.

1. No GitHub, abra **Actions → iOS unsigned laboratory** e escolha uma execução do commit que deseja testar; se necessário, acione `workflow_dispatch`. Confira que o job `ios` passou. Mudanças somente neste documento não disparam necessariamente esse workflow (veja os filtros `paths` no YAML).
2. Baixe **`BBHost-unsigned-arm64`** em **Artifacts** e extraia o ZIP **localmente** no Windows. Confirme que há `BBHost-unsigned.ipa` e os arquivos de metadados. Compare `revision.txt` com o commit da execução e confira `architecture.txt`/`platform.txt` para identificar o alvo compilado.
3. Confira o SHA-256 do IPA contra `SHA256SUMS.txt`, por exemplo em PowerShell: `Get-FileHash .\BBHost-unsigned.ipa -Algorithm SHA256`. Uma correspondência detecta divergência no download; não autentica o conteúdo ou certifica a execução.
4. **Pare** se não houver artefato, se o hash divergir, se a arquitetura não for arm64, ou se a ferramenta de sideload rejeitar o IPA. Registre mensagem de erro, revisão, ambiente e ferramenta, sem incluir dados pessoais ou arquivos do jogo. Não renomeie outro artefato para seguir estas instruções.

O workflow usa `CODE_SIGNING_ALLOWED=NO`; o IPA resultante **não está assinado** e não deve ser apresentado como pronto para instalar. O CI não recebe Conta Apple, certificados nem conteúdo do jogo.

## 2. Preparação local e conta gratuita

A [Apple explica as limitações do Personal Team](https://developer.apple.com/help/account/basics/about-your-developer-account/): até 10 App IDs, três dispositivos e três apps por dispositivo; perfis de provisionamento expiram sete dias após emissão. A documentação da Apple descreve principalmente o provisionamento via **Xcode**; ela **não certifica** que um IPA não assinado produzido em CI será aceito por uma ferramenta Windows. Renovação e requisitos da ferramenta escolhida devem ser confirmados na prática.

- Use um iPhone próprio e um computador Windows confiável, cabo USB adequado e uma Conta Apple sob seu controle. Conecte, desbloqueie e aceite **Confiar neste computador** se o iPhone solicitar.
- Se solicitado pelo sistema, habilite **Modo de Desenvolvedor** em **Ajustes → Privacidade e Segurança** e confirme a reinicialização. Confira também orientações atuais da [Apple sobre Developer Mode](https://developer.apple.com/documentation/xcode/enabling-developer-mode-on-a-device).
- Para **AltStore Classic/AltServer**, siga o [guia oficial para Windows](https://faq.altstore.io/altstore-classic/how-to-install-altstore-windows) para as versões compatíveis de iTunes/iCloud, pareamento, instalação e possíveis exceções da Microsoft Store. Baixe instaladores por links do fornecedor/Apple; não use espelhos não verificados. As exigências podem mudar.
- Ferramentas de terceiros como AltServer ou Sideloadly podem solicitar autenticação da Conta Apple. **Avalie a confiança no fornecedor, leia sua política de privacidade e os riscos antes de inserir credenciais ou códigos 2FA; não compartilhe senha/códigos por chat e nunca os coloque em GitHub Actions, logs ou repositório.** Não há obrigação de usar uma ferramenta específica; se não confiar nela, interrompa o teste.

## 3. Tentativa de instalação no Windows

1. Siga as instruções **atuais** da ferramenta escolhida para instalar seu gerenciador no iPhone e selecionar o `BBHost-unsigned.ipa` obtido na seção 1. No AltStore Classic, a importação de IPA costuma ser feita no próprio app pela aba **My Apps → +**, consulte o [guia inicial do AltStore Classic](https://faq.altstore.io/altstore-classic/your-altstore) e as instruções no app; verifique o procedimento vigente. Se usar outro utilitário, consulte a documentação do fornecedor para assinatura e instalação; não presuma comportamento equivalente.
2. Se aparecer uma solicitação de confiança para o desenvolvedor, consulte **Ajustes → Geral → VPN e Gerenciamento de Dispositivos** e confirme apenas a sua própria identidade. A interface e os avisos variam conforme o iOS e o método de assinatura.
3. Tente abrir o **BB Lab**. O primeiro teste é somente a abertura do app de laboratório com dados/fixtures próprios e observação do que a interface realmente mostra. Não copie `Image0`, `eboot`, módulos, dumps ou assets para esse teste inicial. Uma tela, mensagem ou botão de Metal não demonstra suporte a Metal 3: registre separadamente a detecção de capacidade e o resultado real de renderização, se ocorrer.
4. Anote versão do iOS, modelo, revisão (`revision.txt`), versão da ferramenta, horário, método de assinatura, resultado da instalação e da abertura, erros e limitações. Se falhar, pare e investigue sem afirmar que o sideload está validado.

A assinatura pessoal não remove sandbox, restrições de memória, assinatura de código, entitlements ou diferenças de CPU x86-64/arm64. **Mesmo que o app abra, isso não inicia Bloodborne nem comprova jogabilidade.**

## 4. Expiração, renovação e dados

A Apple informa que o perfil do Personal Team dura sete dias; acompanhe a data de emissão e consulte a documentação da ferramenta para tentar renovar antes do vencimento. Não prometa renovação automática: ela pode depender do Windows/AltServer, rede, pareamento, conta e versão instalada. Ao expirar, refaça o provisionamento conforme a ferramenta exigir e teste novamente a abertura.

**Não há garantia de preservação dos dados do contêiner** entre assinatura, reinstalação, alteração de bundle ID ou remoção do app. Antes de qualquer atualização, mantenha cópia **local e autorizada** dos seus dados de teste não sensíveis, se necessário, e confira no aparelho o que permaneceu. Não desinstale presumindo que `Documents` será preservado. Saves, relatórios e arquivos do jogo não devem ser usados como prova nesta etapa inicial.

## 5. Arquivos locais e diagnósticos — somente após validação do app

O `host/ios/Info.plist` atual declara `UIFileSharingEnabled` e `LSSupportsOpeningDocumentsInPlace`, mas a visibilidade em **Compartilhamento de Arquivos** (Dispositivos Apple/iTunes) ou no app **Arquivos** e a leitura pelo VFS devem ser verificadas **no build efetivamente instalado**. Essas flags não provam importação funcional nem autorizam upload de conteúdo proprietário. Para um teste posterior, use primeiro um arquivo texto autoral inofensivo; confirme presença, leitura e isolamento no dispositivo. Não envie dumps, `Image0`, módulos, `eboot`, shaders extraídos ou assets ao CI, GitHub, serviços de nuvem ou terceiros. Não há necessidade desses arquivos para testar o sideload.

O código-fonte `host/ios/main.mm` contém uma ação de exportação de relatório, mas **não assuma que ela existe ou funciona no IPA até compilar e testar no aparelho**. Também não presuma que o JSON esteja anonimizado ou pronto para compartilhar: revise-o localmente, pois pode conter informações de ambiente, caminhos, conteúdo de fixtures ou dados pessoais. Mantenha relatórios brutos locais; se precisar relatar um problema, redija manualmente um resumo mínimo sem credenciais, dumps, assets ou dados identificáveis.

## Referências

- [Apple Developer — visão geral da conta e limites do Personal Team](https://developer.apple.com/help/account/basics/about-your-developer-account/)
- [Apple Developer — habilitar Developer Mode](https://developer.apple.com/documentation/xcode/enabling-developer-mode-on-a-device)
- [AltStore Classic — instalação no Windows](https://faq.altstore.io/altstore-classic/how-to-install-altstore-windows) e [guia inicial](https://faq.altstore.io/altstore-classic/your-altstore)
- [GitHub Docs — baixar artefatos de uma execução](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/download-workflow-artifacts)

Consulte as versões atuais dessas páginas antes de seguir o procedimento; nomes de menus, ferramentas e regras podem mudar.
