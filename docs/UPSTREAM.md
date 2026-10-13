# Referências fixadas

Checkout separado, sem copiar fontes upstream para este laboratório:

| Projeto | Branch consultada | Commit |
|---|---|---|
| `Supermedo/bloodborne_pc` | `windows` | `e5d04e2ba4864ccf40de6c3cf6aa532855320b0d` |
| `morluto/rea` | `main` | `4aa51bfe1997ef2df6e905ab2fb7bcd5fabfe16e` |

As revisões acima não comprovam correspondência com os binários do pacote Windows já existente. Os clones estão em `../upstream/`, fora da raiz publicável.

No port, `src/probe.c:11` exige x86-64/GNU, `src/probe.c:55` chama VirtualAlloc no caminho Windows, `src/probe.c:95` contém assembly de entrada, e `src/probe.c:700`/`:706` transferem controle para `image + entry`. Referências vinculadas ao commit acima. Isso fundamenta a separação do laboratório e impede tratar recompilação arm64 como solução completa.

Gitlinks registrados por `git ls-tree -r HEAD` (submódulos ainda não inicializados):

| Caminho | Commit |
|---|---|
| `gpu/third_party/fsr-vulkan` | `c64f093404125e960813f95f1996c14780f0dd69` |
| `gpu/third_party/imgui` | `f1cc2ae15e53a861a874c3034aae6798fde194ab` |
| `gpu/third_party/magic_enum` | `e046b69a3736d314fad813e159b1c192eaef92cd` |
| `gpu/third_party/miniz` | `2948eebafc5b9f1c69bebed80333e403a09bd62d` |
| `gpu/third_party/xbyak` | `cd6bad3cad2e07a3e277c23d2b1f065320024717` |
| `third_party/LibAtrac9` | `efca2e3af35562a09a9bb6deed90e45b4b824dc4` |

REA: o guia `docs/windows-ghidra-p0.md` da revisão fixada limita o provider Windows experimental a PE nativo e raw NMOS 6502, não PS4. Node, Java e Ghidra não foram instalados nesta etapa. Não foi realizado `doctor` ou análise com provider.

Toolchain host: w64devkit 2.10.1 oficial, GCC 16.2.0, CMake 4.4.4. SHA-256 do download: `add1edf984ce4d473cfeb7c52e0ab38db73a855bba6c7f564d5e16648eb2896a`, comparado ao digest da API oficial de releases antes da extração. Nenhuma alteração persistente no PATH.
