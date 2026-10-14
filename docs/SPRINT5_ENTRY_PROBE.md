# Sprint 5 - tentativa controlada do ponto de entrada do eboot

Objetivo: executar de forma interpretada e limitada as duas primeiras instrucoes x86-64 do `eboot.elf` local no iPhone arm64, sem JIT nem stub de entrada. Isto nao e boot da engine nem jogabilidade.

## Fonte local e privacidade

Referencia no PC: `bloodborne_portPC/bbport-windows/out/eboot.elf`, 93.667.640 bytes, SHA-256 `cec1b276e7f9e4db978e57f524f41fbaac594530a3437b002e23f3fab14b4f86`. Artefato derivado do jogo, nao enviado ao GitHub/CI e nao incluido no IPA. Transferir somente uma copia diretamente do PC para `Documents` do BB Lab pelo iTunes/Dispositivos Apple. Nome exato: `eboot.elf`.

## Procedimento e criterio

O app le somente 4 KiB de cabecalho ELF e 16 bytes do ponto de entrada, limite de arquivo de 128 MiB. Valida ELF64 x86-64 e segmento executavel, aloca pilha guest isolada e interpreta apenas `push rbp` e `mov rbp,rsp`, parando no proximo opcode sem chama-lo. Esperado para esta fonte local: `entry_rip=160` (`0xA0`), `instructions_executed=2`, `stop_rip=164` (`0xA4`), `stop_opcode=65` (`0x41`), `status=stopped_after_two_instructions`, `entry_reached=true`, `game_execution=false`.

Tocar em **Testar entrada x86-64 (2 instrucoes)** e depois em **Exportar diagnostico de estabilidade**. O JSON contem `boot_probe`. A exportacao repete o teste. Falhas de formato, segmento, limites, arquivo ausente e opcode desconhecido param antes de qualquer chamada externa.

Teste host sintetico cobre arquivo ausente, dois opcodes conhecidos, opcode desconhecido e ELF invalido. `BB_TEST_EBOOT_DIR` permite teste local opcional contra o executavel real sem copia para o repo; CI executa apenas fixtures sinteticas.

## Limites

A sonda nao carrega os segmentos completos, nao resolve imports, nao executa AOT em lote, nao faz chamadas HLE do jogo e nao transfere assets. O `GuestLoader` existente possui um handshake simulado que nao faz parte desta prova. `game_execution=false` permanece o indicador honesto ate um boot real posterior.
