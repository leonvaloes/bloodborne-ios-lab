# Orçamento de memória guest — estado e limites

Em 15/10/2026, `GuestMemory` mantém regiões em `std::vector<uint8_t>`, inicializadas com zeros ao mapear. `budget()` limita os **bytes virtuais de regiões guest mapeadas**; `used()` e `peak_used()` registram atual e pico sob mutex. Não medem RAM residente, Metal, assets, heap C++ nem limites de jetsam do iOS. Um orçamento configurado de 768 MiB em `host/ios/main.mm` **não é** reserva total do processo e reduzi-lo arbitrariamente para 512 MiB pode causar falha de boot sem reduzir necessariamente o pico físico.

`madvise(MADV_FREE)` seria incorreto sobre buffers vivos: o guest pode reler dados descartados. `mmap`/alocação lazy e pools de textura exigem testes de semântica de zero-fill, permissões, atomicidade, desmapeamento, concorrência e pressão de memória em hardware físico antes de substituir o backend atual.

Experimento seguinte: medir separadamente `used()/peak_used()`, footprint físico de processo e orçamento de texturas durante fixtures autorais e no aparelho (somente com dados fornecidos localmente pelo usuário); registrar ambiente, revisão, procedimento, esperado, observado e limitações. Não declarar limite seguro de jetsam apenas pela RAM nominal do dispositivo.
