# Protocolo de validação física — pendente

Dispositivo informado: iPhone 15 Pro Max. Versão do iOS ainda não informada. AltServer não está instalado, conforme o usuário.

## Antes da instalação

1. Obter um job `iOS unsigned laboratory` verde e baixar `BBHost-unsigned-arm64`.
2. Verificar SHA-256 do IPA contra `SHA256SUMS.txt`, revisão e plataforma iOS arm64.
3. Preparar AltServer e dependências conforme o [guia oficial para Windows](https://faq.altstore.io/altstore-classic/how-to-install-altstore-windows). Revisar requisitos vigentes antes de instalar; as credenciais Apple são fornecidas pelo usuário diretamente ao fluxo escolhido.
4. Conectar o aparelho, autorizar o computador e concluir assinatura/instalação pessoal. Um IPA sem assinatura não é instalável por si só.

## Execução e aceite

- Abrir BB Lab e executar CPU/memória. Esperado: 14 verificações, zero falhas.
- Confirmar visualmente o triângulo vermelho e contador de quadros Metal crescente. O contador indica conclusão GPU; sozinho não prova apresentação visual correta.
- Exportar diagnóstico pelo botão, preservar o JSON com build, modelo, iOS, timestamp e resultados.
- Repetir após fechar/reabrir e reiniciar o aparelho. Repetir sem PC/rede com assinatura válida.
- Verificar renovação e abertura após o ciclo de validade do provisionamento, registrando as datas reais.
- Registrar qualquer erro sem divulgar senha, token, Apple ID ou perfil.

Somente esses testes podem fornecer evidência de S3 e de parte do experimento CPU/Metal. Não validam Bloodborne, F1 completa ou renderer Vulkan.

Implementação usa [MTKView](https://developer.apple.com/documentation/metalkit/mtkview); o build separado iOS segue a [documentação de toolchains do CMake](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html).
