# Evidência de execução no iPhone — laboratório

## 16/10/2026 — relatório fornecido pelo usuário

- **Origem e revisão:** JSON de diagnóstico exportado pelo usuário do BB Lab; `build=5f2b0f7c9a426dd894ce6d3318dfd4726fa8db77`, `schema=1`, horário declarado `2026-10-16T11:15:00Z`. Não foi recebido log de instalação, assinatura nem captura de tela.
- **Ambiente reportado:** `iOS-device`, `iPhone16,2` (iPhone 15 Pro Max), iOS `26.6.2`, `Apple A17 Pro GPU`. O relatório indica Metal 3/Apple9 e memória unificada; versão da ferramenta de sideload e tipo de assinatura não foram informados.
- **Procedimento conhecido:** o usuário instalou/abriu o laboratório e exportou o relatório. Duração exata, forma de instalação, interação com controles e áudio audível não foram documentadas. Não foram fornecidos arquivos do jogo para este teste.
- **Esperado para o teste de infraestrutura:** abrir o app, apresentar a cena sintética no Metal e permitir exportar diagnósticos sem dados do jogo.
- **Obtido no relatório:** `metal_completed_frames=2504`, `average_fps=59.984264148033247`, `frame_pacing_p50_ms=16.6743335`, footprint físico pontual `109.64141082763672 MB`; `cpu_checks=118`, `cpu_failures=0`; malha `synthetic_saw_cleaver` com 8 vértices/12 triângulos; diretório Documents disponível e VFS montado. `audio_status=ready` indica inicialização, **não** saída audível confirmada.
- **Ausências/limitações:** `video_flips=0`, `guest_steps_executed=false`, `game_execution=false`, `boot_probe.status=not_found`, `eboot.bin`/`eboot.elf`/`param.sfo` ausentes na listagem; `gamepad_connected=false`. FPS é da cena host, não do jogo. O dado reportado evidencia execução da infraestrutura no aparelho, mas não demonstra gameplay, tela de título, áudio audível, resposta dos controles, estabilidade prolongada nem assinatura reproduzível. Não marcar S9 concluído.

Próxima medição útil: documentar duração e método de instalação (sem credenciais), verificar visualmente cena/controles e áudio, e repetir relatório após teste prolongado. Não enviar dumps, assets, saves ou logs brutos com dados pessoais ao repositório.
