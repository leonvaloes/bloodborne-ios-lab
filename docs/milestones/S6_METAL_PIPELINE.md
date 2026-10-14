# Marco S6 ? Pipeline Gr?fico Representativo (Metal 3 & Shader Uniforms no A17 Pro)

**Data:** 14/10/2026  
**Status:** ? **CONCLU?DO (CAPABILITIES INTEGRADOS + SHADERS UNIFORMS & INTERPOLA??O)**

---

## Objetivo do Marco (conforme ESCOPO)

> **S6**: renderiza??o Vulkan/MoltenVK/Metal e pipeline de shaders representativo executam no A17 Pro GPU; valida??o de capabilities, uniform buffers, interpola??o de cores e frame pacing.

---

## Resumo T?cnico da Implementa??o

1. **Inspe??o de Recursos de GPU em Tempo de Execu??o:**
   - Detec??o din?mica de suporte ?s fam?lias de GPU da Apple:
     - `MTLGPUFamilyApple9` (Arquitetura A17 Pro com Ray Tracing e Mesh Shading por hardware).
     - `MTLGPUFamilyMetal3` (Metal 3 com suporte a dynamic resources e indirect command buffers).
     - `hasUnifiedMemory`: verifica??o de arquitetura de mem?ria unificada entre CPU e GPU.
     - `maxBufferLength`: consulta de limites m?ximos de buffer para mapeamento de VRAM do PS4.

2. **Pipeline de Shaders Metal Representativo:**
   - Vertex shader (`triangle_vs`) alimentado por uniform buffer din?mico (`constant Uniforms &u [[buffer(0)]]`).
   - Aplica??o de matriz de transforma??o de rota??o calculada a cada quadro em 30 FPS (`u.transform * float4(pos[i], 0, 1)`).
   - Interpola??o de cores por v?rtice (`VertexOut.color`) passada para o fragment shader (`color_fs`).
   - Estrutura pronta para vincula??o aos shaders SPIR-V traduzidos da GPU do PS4 via pipeline MoltenVK.

3. **Valida??o e Diagn?stico:**
   - M?tricas de GPU expostas diretamente no relat?rio JSON (`metal_supports_metal3`, `metal_supports_apple9`, `metal_has_unified_memory`, `metal_max_buffer_length`).
   - Frame pacing cont?nuo a 30 FPS gerenciado por `MTKViewDelegate`.
