# Estado verificável da ponte GNM → Metal

Em 15/10/2026, o laboratório possui decodificação PM4, rasterizador de referência em CPU para um subconjunto e tradução GCN→MSL de um subconjunto muito pequeno de instruções. **Não há backend Metal offscreen funcional para draws PM4 do jogo.** O `BBHost` apresenta pixels BGRA8 lineares lidos do guest pela `VideoOutManager`; o triângulo FLVER é uma demonstração separada. Um IPA unsigned compilado não confirma instalação nem execução em um iPhone.

A tentativa anterior de conectar `GnmMetalBridge` diretamente ao `drawInMTKView:` foi revertida por ser insegura/incompleta:

- o encoder de demonstração não copiava vértices/índices/texturas da memória guest, criava recursos vazios e substituía draws indexados por draws não indexados;
- os nomes de entrada usados pelo host não correspondiam aos símbolos `gcn_vertex` e `gcn_fragment` produzidos pelo tradutor;
- o método fazia `waitUntilCompleted` na thread de apresentação e consultava `GnmContext` simultaneamente à thread guest, sem snapshot/sincronização;
- não havia associação comprovada entre render target PM4 e buffer de flip, nem gestão de vida útil/ordem dos recursos Metal;
- o commit `25a4d9f` incluiu somente o arquivo Objective-C++ isolado; as alterações de CMake/host/bridge permaneceram locais e não faziam parte do IPA daquele commit.

Próximos experimentos autorais, por ordem: (1) capturar batches imutáveis no momento da submissão, com limites explícitos e cópia validada de recursos; (2) testar snapshots de estado e índices com fixtures PM4 sintéticas; (3) implementar encoder offscreen com pipeline/attachment compatíveis, sem leitura concorrente de memória guest; (4) inspecionar resultado de GPU em runner macOS e, só depois, em iPhone físico; (5) acrescentar telemetria que diferencie draw enviado, command buffer concluído, pixels lidos e quadro efetivamente apresentado. Não descrever essas etapas como concluídas até haver evidência.

O cache de tradução na ponte agora distingue estágios VS/PS/CS para bytecodes idênticos; valida buffer guest vinculado e offset do índice. Isso é validação host, **não** renderização do jogo.
