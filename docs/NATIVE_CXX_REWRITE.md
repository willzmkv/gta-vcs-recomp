# Engenharia Reversa Progressiva em C++ Nativo (`exp/native-cxx`)

**Branch:** `exp/native-cxx` (local e `origin/exp/native-cxx`, derivado de `agent/issue-113` após as rodadas 1–5 de `#113`).  
**Preservação:** `origin/hd-world-merge` é o **backup estável e branch de releases públicas (minor releases apenas)**. **Nunca mesclar trabalho experimental em `hd-world-merge` sem validação completa e aprovação explícita do usuário.**

---

## Contrato Obrigatório para Todos os Agentes

1. **O que estamos fazendo:** substituição progressiva por engenharia reversa (reimplementação em C++ x64 nativo) dos módulos quentes do jogo (EE / colisão / renderização / simulação), mais melhorias estruturais no recompilador/runtime (`ps2xRecomp`, arena virtual de memória, GPU skinning, submissão RSL), mantendo a semântica do jogo original de PS2.
2. **Regra 1 de `AGENTS.md` (Fallback & Paridade):** toda substituição nativa ou otimização estrutural **DEVE** manter o caminho emulado/anterior acessível por variável de ambiente (`VCS_...=0` / `PS2X_...=0`) e ser validada antes com verificador bit-a-bit (`*_VERIFY=1`) ou captura de cenas (`tools/scenes.py compare`), além de `tools/build.py --opt lto` ao medir.
3. **Registro Obrigatório Passo a Passo:** ao concluir (ou abortar/reverter) qualquer subpasso listado neste documento, **atualize obrigatoriamente**:
   - A tabela de **Progresso por Passo** abaixo;
   - A seção **Diário Técnico de Passos Executados** com: data, endereço/função PS2 investigada, arquivos alterados, flag de fallback (Rule 1), método e contagem exata de paridade (`VERIFY`, `selftest`, `scenes`), números A/B antes/depois em ThinLTO (`--opt lto`) nas cenas-referência (Little Havana `redwood_rain`, `police`, `bench_avenue`, prints F3), e eventuais armadilhas descobertas;
   - `docs/game-internals.md` (estruturas, offsets e endereços descobertos) e `docs/PS2RECOMP_PATCHES.md` (se tocar em `third_party/PS2Recomp`).
4. **Não perder foco por micro-otimizações que não movem o gargalo:** olhar sempre para onde o profiler + `F3` mostram o tempo real. Não gastar um ciclo otimizando 0.5 ms na GPU/MainThread se a `GameThread` está em `12–14 ms` (`85%+` da barra laranja do `F3`).

---

## Baseline Inicial do Branch (`2026-11-25`, pós-#113 Rodadas 1–5)

### Por que 4K Little Havana ainda ficava em ~58–72 fps?
Medições reais do usuário (3 screenshots com `F3`, `4K`, `HD World R=3`, população 100%, `Crowd Bands = 1`, mods, aniso 16) + ThinLTO `--opt lto` automatizado:

| Cenário (4K, mods, R=3) | FPS | Frame | `game` (laranja) | `render` (azul) | `wait` | `host` | `gpu` | Draws | Peds / Carros |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| Print usuário #1 (`PCJ600` 00:48 NW) | **61.9** | 16.1 ms | **14.1 ms** | 1.3 ms | 0.5 ms | 14.9 ms | 5.0 ms | 4530 | 86 / 67 |
| Print usuário #2 (`PCJ600` 00:11 Redwood NE) | **58.1** | 17.2 ms | **14.1 ms** | 1.8 ms | 1.2 ms | 15.8 ms | 5.7 ms | 5105 | 72 / 65 |
| Print usuário #3 (`Admiral` 00:13 Redwood NE) | **72.5** | 13.8 ms | **12.1 ms** | 1.3 ms | 0.2 ms | 6.0 ms | 2.6 ms | 4359 | 99 / 71 |
| Auto `redwood_4k_d3d12_uncap` (`14:32` chuva) | **71.5** | 14.0 ms | **12.3 ms** | 1.5 ms | 0.0 ms | 5.1 ms | 1.9 ms | 4160 | 119 / 44 |
| Auto `redwood_4k_vulkan_uncap` (`14:31` chuva) | **68.1** | 14.7 ms | **12.9 ms** | 1.5 ms | 0.0 ms | 5.3 ms | 2.2 ms | 4113 | 111 / 43 |
| Auto `police_4k_d3d12_uncap` | **99.9** | 10.0 ms | 8.8 ms | 1.0 ms | 0.0 ms | 5.2 ms | 1.7 ms | 2734 | 30 / 41 |
| Auto `police_4k_vulkan_uncap` | **107.9** | 9.3 ms | 8.2 ms | 0.9 ms | 0.0 ms | 4.6 ms | 1.9 ms | 2746 | 32 / 44 |
| Auto `fortbaxter_lh_4k_vulkan` | **99.0** | 10.1 ms | 7.6 ms | 1.4 ms | 1.0 ms | 6.8 ms | 2.4 ms | 2497 | 73 / 43 |
| `bench_avenue.py` D3D12 (2 rodadas) | **75.5** | 13.24 ms | 11.25 ms | — | — | 5.37 ms | — | — | 88 (44 <200m) |
| `bench_avenue.py` Vulkan (2 rodadas) | **72.2** | 13.86 ms | 12.45 ms | — | — | 5.04 ms | — | — | 91 (45 <200m) |

### Perfil real de gargalos (`VCS_PROFILE` em Little Havana `redwood_rain`)
- **`GameThread` (~85–90% do tempo de quadro nos prints do usuário, `12.1–14.1 ms`):**
  - `63.9%` em código guest recompilado (`generated/ee/`) em 1 thread (`CWorld::Process 0x1933c4` `~29%` incl., `CRenderer 0x35d610` `~21%` incl., `CGame::Process 0x3ba904` `~15%` incl.).
  - Principais focos individuais observados antes da Fase 1:
    1. **Colisão & Física:** `FUN_002b8ed0` (`11.1%` amostras de folha no profiler recente + scratchpad `0x70002020`), `sphereTriangle` (`5.5%` + `segmentTriangleFrom0068` `1.7%` = `7.2%` em `vcs_vu0_native.cpp`), `FUN_00348d44`/`0x34fd60` (`~8.4%` incl.), `CAutomobile::ProcessControl` (`0x13a0a0`, `4.4%`), `CPed::ProcessControl` (`0x1f4bf0`, `~8.8%` incl.), `entitySphereTestNative` (`0x3c4170` + `ps2_fadd_slow`/`ps2_fmul_slow` `~2.5%`). *(Nota: `0x39dfe0`/`0x39e050` `3.2%` já receberam port nativo no commit base `3f9cc36`.)*
    2. **Render no lado do jogo (`GameThread`):** `FUN_0039EC18` (skinned clumps + paletas de ossos + pacotes, `15.2%` inclusivo / `5.3%` próprio), `CRenderer` traversal (`0x35cd98` `15%`, `0x36dbf8` `9.4%`, `0x36eb70` `8.6%` incl.), `FUN_002F62B8`/`0x2F6A78` (`~2.5%` próprio).
    3. **Overhead estrutural do código recompilado:** `ps2ReadChecked<uint32_t>` (`2.2%`), checks `if (addr < g_ps2FastReadLimit)` em cada leitura/escrita RAM, `ctx->r[]`/`ctx->f[]` em memória, `eeCheckpointDue()` em loops, `FUN_002560D8` VM de scripts (`3.0%`).
- **`RenderWorker` (~80% ocupada em paralelo, gera `1.3–1.8 ms` de `render` sync na `GameThread`):**
  - `24.1–26.5%` `memcpy`, `5.9–7.5%` `beforeNative`, `4.1–4.3%` `skin`, `2.0–3.4%` `memcmp`, `2.6–2.8%` `setupMatrices`/`drawNativeRigid`.
  - **Por que GPU Skinning sozinho não destrava FPS maciço hoje:** tira apenas os `4.1–4.3%` do `RenderWorker` (< 1 ms paralelo) e parte dos `memcpy`, sem tocar nos `12–14 ms` da `GameThread`. Faz sentido na Fase 4 (ou após derrubar a `GameThread`).

---

## Plano Passo a Passo (Fases 1 a 4)

| Fase / Passo | Alvo Principal | Meta na `GameThread` / Frame | Status |
|---|---|---|---|
| **1.1** | `entitySphereTestNative` (`0x003C4170` fast normal-float SSE) + `vcs_vu0_native.cpp` (`CollisionVu` em registradores SSE locais, `dotW`, `clipW`, `convertInputs`) | `-0.8 a -1.5 ms` | `concluido` |
| **1.2** | Engenharia reversa (`eedecomp.py 0x2b8ed0 0x34fd60 0x348d44`) + port nativo C++ SIMD de `FUN_002b8ed0` (bypass total de scratchpad/VU0 marshaling na colisão mesh/setor) | `-1.5 a -2.2 ms` | `concluido` |
| **1.3** | Engenharia reversa & port nativo de `FUN_0034FD60` / `FUN_0012A278` / `FUN_002B7A10` (varredura de colisão de entidades/setores) | `-1.0 a -1.5 ms` | `pendente` |
| **2.1** | Engenharia reversa (`eedecomp.py 0x39ec18 0x2f62b8 0x2f6a78`) + substituição nativa C++ de `FUN_0039EC18` e pipeline RSL (`vcs_rsl_render_native.{h,cpp}`) + fast paths em `vcs_rsl_native.cpp` (`emitCfae0`/plugins) | `-1.5 a -2.5 ms` | `concluido` |
| **2.2** | Engenharia reversa & port nativo de `CRenderer` (`0x35d610`, `0x35cd98`, `0x36dbf8`, `0x36eb70` frustum/traversal) | `-1.0 a -2.0 ms` | `concluido` |
| **2.3** | Simulação & Crowd/Vehicle LOD adaptativo (`0x13a0a0`, `0x1f4bf0`, pool `65–71` veículos / `72–99` peds fora do raio próximo) | `-1.0 a -2.0 ms` | `pendente` |
| **3.1** | Memória Virtual Direta (`VirtualAlloc`/`mmap` + guard pages / fast arena) para eliminar `if (addr < limit)` + `PS2_RAM_MASK` de todo `READ`/`WRITE` em `generated/` | `-1.5 a -2.5 ms` | `pendente` |
| **3.2** | Otimização da Script VM (`FUN_002560D8` despacho direto / fast-path) | `-0.3 a -0.5 ms` | `pendente` |
| **3.3** | Melhorias de codegen no `ps2xRecomp` (caching local de registradores GPR/FPR quentes, eliminação de `eeCheckpointDue` redundante em leaf/pequenos loops, fast-FPU onde seguro) + `--recompile` | `-2.0 a -3.5 ms` | `pendente` |
| **4.1** | **GPU Skinning** (`CROWD_BANDS_PLAN.md` Passo 6: índices/pesos no VB estático + paleta de ossos em cbuffer no `vs_lit` / `vs_lit_inst`, removendo `skin()` da `RenderWorker`) | `-render sync & p99` | `pendente` |
| **4.2** | Submissão RSL direta na `RenderWorker` (`RSL_SUBMISSION.md`: matar cópias/replay VIF1 em `beforeNative` + `25% memcpy`) | `-render sync & p99` | `pendente` |
| **4.3** | Treino final de **PGO (`--opt pgo --train`)** após estabilização da cadeia nativa | polish final (`+5-15%`) | `pendente` |

---

## Como Executar Cada Fase (Guia Técnico para Agentes)

### Fase 1: Colisão & Física Nativa (`src/game/vcs_overrides.cpp`, `src/game/vcs_vu0_native.cpp`, novo `src/game/vcs_collision_native.cpp`)
1. **Passo 1.1 (Kernels VU0 & `entitySphereTest`):**
   - Em [vcs_overrides.cpp](file:///D:/Games/vcs-agent/src/game/vcs_overrides.cpp), `entitySphereTestNative` (`0x003C4170`) hoje chama 22 operações `ps2_fadd`/`ps2_fsub`/`ps2_fmul` escalares que saltam para `ps2_fadd_slow`/`ps2_fmul_slow` sempre que coordenadas locais do `CColModel` são zero (`cx=0`, `r0y=0`). Substituir `evalNative` por cálculo SSE direto (`_mm_mul_ps`/`_mm_add_ps`, `-ffp-contract=off` respeitado) após checar se algum operando não-zero tem expoente `0` ou `255`, com fallback para caminho escalar quando houver número especial. Testar com `VCS_NATIVE_SPHERE_TEST_VERIFY=1` (`0 diffs`).
   - Em [vcs_vu0_native.cpp](file:///D:/Games/vcs-agent/src/game/vcs_vu0_native.cpp), otimizar `CollisionVu`: substituir `dotW` (que faz 3 idas/voltas de RAM em `r.acc` e `r.vf[]`), vetorizar `clipW` (`_mm_cmpgt_epi32`/`_mm_movemask_ps`), e vetorizar `convertInputs` em registradores locais antes de gravar `r.vf[14..17]`. Sempre checar com `PS2X_VU0_VERIFY=1` (`0 differed`).
2. **Passo 1.2 (Engenharia reversa & port de `FUN_002b8ed0`):**
   - Decompilar `0x2b8ed0`, `0x34fd60`, `0x348d44`, `0x2b7a10`, `0x2b7e88`, `0x2b8508` com `tools/eedecomp.py` para `build/analysis/decomp/`.
   - Documentar em `docs/game-internals.md` o layout exato de `CColModel`, lista de caixas/esferas/triângulos comprimidos (`int16_t` 1/128 e normal 1/4096), cache de scratchpad `0x70002020..`, e quais valores/registradores os chamadores realmente leem na saída de `0x2b8ed0`.
   - Criar `VCS_NATIVE_COL_VERIFY=1` (comparando resultado de contato, normal, profundidade, `a0..a3` e lista de contatos com `FUN_002b8ed0_0x2b8ed0`) e `VCS_NATIVE_COL=0` (fallback Rule 1), mantendo `ctx->pc != entry` encaminhado para `g_orig` se houver retomada de checkpoint.
   - Reimplementar o loop de teste de triângulos diretamente em C++ x64 AVX2/SSE sem copiar nada ao scratchpad nem invocar `0x39DFE0`/`0x39E050`/VU0.
3. **Passo 1.3 (`FUN_0034FD60` / `FUN_0012A278`):**
   - Levantar a varredura de entidades na grade de colisão (`0x34FD60` / `0x12A278`) para C++ nativo com checagens AABB/esfera em SoA/SIMD local, mantendo exatamente a mesma ordem de resposta de colisão e desativando via `VCS_NATIVE_COL=0`.

### Fase 2: Renderização do Lado do Jogo & Simulação (`CRenderer` / `FUN_0039EC18` / Crowd LOD)
1. **Passo 2.1 (`FUN_0039EC18` — Skinned Clump Render & Paleta):**
   - Decompilar `0x39EC18` (observado em `PERFORMANCE.md 48` + `15.2%` incl. no profiler).
   - Mapear como ela lê hierarquia de ossos, calcula matrizes 3x4, aplica luzes (`0x520860..`), e chama `FUN_002F7D58`/`0x2F62B8`/`0x2F6A78` para gravar pacotes.
   - Criar port nativo C++ (`VCS_NATIVE_SKIN_SUBMIT=0` fallback, `VCS_NATIVE_SKIN_SUBMIT_VERIFY=1`) que escreve os mesmos itens/matrizes/pacotes (ou itens RSL diretos em `vcs_rsl_world` / novo `vcs_rsl_models.cpp`) sem overhead do guest.
2. **Passo 2.2 (`CRenderer` `0x35d610`, `0x35cd98`, `0x36dbf8`, `0x36eb70`):**
   - Decompilar e instrumentar quantas entidades são varridas por frame em Little Havana (`r=3`, `4500–5100` draws vs `2160` nas docas).
   - Portar o culling/frustum/list builder de `CRenderer` para C++ nativo (`VCS_NATIVE_RENDERER=0`), verificando com `tools/scenes.py compare` + capturas em Little Havana e `hud_check`/`hdworld_missions`.
3. **Passo 2.3 (Simulação / Crowd & Traffic LOD):**
   - Investigar por que os prints reais em Little Havana mantêm `65–71` veículos e `72–99` peds mesmo com `Crowd Bands = 1`: usar `distancestate` / `popview` / `VCS_PROFILE` para medir `CAutomobile::ProcessControl` (`0x13a0a0`) e `CPed::ProcessControl` (`0x1f4bf0`), e aplicar LOD de simulação seguro além da zona próxima (sem afetar missões, carros perseguidores/scripts ativos, ou causar pops visíveis).

### Fase 3: Recompilador (`ps2xRecomp`) & Memória Virtual Direta
1. **Passo 3.1 (Arena Virtual Direta para RDRAM + Scratchpad):**
   - Avaliar `VirtualAlloc` (Windows) / `mmap` (Linux) contíguo ou base fixa mapeada com guard pages de `PS2_RAM_SIZE + ...` para que `READ`/`WRITE` no código gerado deixe de testar `addr < g_ps2FastReadLimit` em cada instrução (protegendo compatibilidade com `PS2X_WRITE_WATCH` e MMIO/Scratchpad `0x70000000`).
2. **Passo 3.2 (`FUN_002560D8` VM Dispatch):**
   - Otimizar o loop/despacho de opcodes em `vcs_overrides.cpp` preservando `VCS_TRACE_TELEPORT`/`VCS_TRACE_PED_REMOVE` e extensão de sinal de retorno.
3. **Passo 3.3 (Codegen em `third_party/PS2Recomp/ps2xRecomp/`):**
   - Registradores em variáveis locais dentro de blocos/funções folha (menor tráfego em `ctx->r[]`/`ctx->f[]`), redução de `eeCheckpointDue()` em loops curtos garantidos, e fast-path FPU (`ps2_fadd`/`ps2_fmul`) configurável por opção preservando estritamente `VCS_SELFTEST=1` e `LAN_B6` (`#103`).

### Fase 4: `RenderWorker`, GPU Skinning & PGO Final
1. **Passo 4.1 (GPU Skinning — `CROWD_BANDS_PLAN.md` Step 6):** mover matriz de ossos (4x3 ou 4x4, até N ossos por draw) para cbuffer dedicado no `vs_lit`/`vs_lit_inst` e atributos de índice/peso no VB estático da malha (`PS2X_GPU_SKIN=0` fallback), validado com `PS2X_HLE_VERIFY` / comparativo de pixels.
2. **Passo 4.2 (RSL Direto na `RenderWorker` — `RSL_SUBMISSION.md`):** eliminar o rebuild de memória VU no `beforeNative`/`replay`.
3. **Passo 4.3 (`--opt pgo --train`)** na árvore estabilizada + bateria completa (`scenes`, `tour`, `bench_avenue`, `redwood_rain`, `framerate_audit`, `fire_check`, `soundcheck`).

---

## Diário Técnico de Passos Executados

### `[2026-11-25]` Passo 0 — Criação do branch e snapshot base (`exp/native-cxx`)
- Commit consolidado das rodadas 1–5 do `#113` salvo em `agent/issue-113` (`3f9cc36`).
- Criado branch `exp/native-cxx` local e publicado em `origin/exp/native-cxx` (`origin/hd-world-merge` mantido intocado em `e676a68` como backup estável/release pública).
- Documento vivo criado em `docs/NATIVE_CXX_REWRITE.md` e referenciado em `AGENTS.md`.
- **Próximo:** Passos 1.1 e 1.2 concluídos; iniciar imediatamente **Fase 2 / Passo 2.1** (`0x0039EC18` skinning/submissão — atual #1 guest hotspot em 4K com `3.4%` + `memcpy 4.0%` / `o_memset 2.4%`) e mapear `0x0032E750`, `0x002560D8`, `0x0023E74C`, `0x002F648C`, `0x00141FE8`, `0x0034FD60` + `vcs::rslworld` (`5.3%` host).

### `[2026-11-26]` Passo 1.1 — Fast-path SSE em `entitySphereTestNative` (`0x003C4170`) e `CollisionVu` em registradores locais

- **Arquivos alterados:** [src/game/vcs_overrides.cpp](src/game/vcs_overrides.cpp), [src/game/vcs_vu0_native.cpp](src/game/vcs_vu0_native.cpp)
- **O que foi feito:**
  - Adicionado fast-path SSE para floats normais em `entitySphereTestNative` (`0x003C4170`) evitando fallbacks desnecessários no caminho rápido.
  - Reescritos helpers quentes de `CollisionVu` em `vcs_vu0_native.cpp` (`dotW`, `clipW`, `convertInputs`) operando em registradores SSE locais em vez de ficar transitando pelos bancos completos do contexto R5900/VU0.
- **Verificação bit-exata:**
  - `[spheretest-verify] checked=196608 diffs=0` (`VCS_NATIVE_SPHERE_TEST_VERIFY=1`)
  - `[xform-verify]` `0x39E050 checked=327680 diffs=0`, `0x39DFE0 checked=16384 diffs=0` (`VCS_NATIVE_XFORM_VERIFY=1`)
  - `[vu0verify]` (`PS2X_VU0_VERIFY=1`): `0x020` (`500000 runs, 0 differed`), `0x3A0` (`580000 runs, 0 differed`), `0x0CF8` (`1160000 runs, 0 differed`), `0x0EB8` (`1120000 runs, 0 differed`), `0x0C10` (`180000 runs, 0 differed`).

### `[2026-11-26]` Passo 1.2 — Port nativo C++ / SSE de `CCollision::ProcessColModels` (`0x002B8ED0`)

- **Arquivos adicionados/alterados:** [src/game/vcs_collision_native.h](src/game/vcs_collision_native.h), [src/game/vcs_collision_native.cpp](src/game/vcs_collision_native.cpp), [src/game/vcs_vu0_native.cpp](src/game/vcs_vu0_native.cpp), [src/game/vcs_overrides.cpp](src/game/vcs_overrides.cpp), [docs/game-internals.md](docs/game-internals.md) (seção 12f).
- **Engenharia reversa e arquitetura:**
  - `0x002B8ED0` (`CCollision::ProcessColModels`) e helpers de caixa/esfera/linha/triângulo (`col_helpers.c`, `col_lru.c`) portados para C++ nativo com entrypoints VU0 diretos sem cópia de bancos (`vuSphereTriangleTest`, `vuSphereTriangleHit`, `vuSegmentTriangleHit`).
  - **Switches de fallback/verificação (Regra 1):** `VCS_NATIVE_COL=0` desativa o port nativo e retorna ao código recompilado; `VCS_NATIVE_COL_VERIFY=1` executa ambos em paralelo e compara resultados bit a bit; `VCS_NATIVE_COL_MAX_TRIS` / `VCS_NATIVE_COL_LINES` disponíveis para ajuste. Fallbacks em runtime restritos a cache frio de normais (`numTris != 0 && pNormals == 0`) ou ponteiros fora do range (`fallbacks=18` em 65536 chamadas).
- **Armadilha crítica resolvida (`1..57 ULP` em `lineT`):**
  - Microprogramas VU0 no PS2 executam em **round-toward-zero** (`MXCSR RC = 3`, `(csr & ~0x6000u) | 0x6000u` em `ps2_runtime.cpp:2171-2174`), enquanto o EE/host roda em **round-to-nearest-even** (`RC = 0`).
  - Ao rodar `vuSphereTriangleTest` / `vuSphereTriangleHit` / `vuSegmentTriangleHit` com arredondamento do host, produtos escalares de interseção divergiam `1..57 ULP`. Aplicado `Vu0RoundGuard` RAII ao redor dos loops de teste VU0 (mantendo `FPU_SUB_S(sA.w, h.dist)` fora do guard, em arredondamento EE) e coletando hits em buffer local antes da subtração EE.
- **Verificação e qualidade (build `--opt lto`):**
  - **Paridade bit-exata (`police` + `havana` + `autodrive`):** `[col-verify] checked=65536 diffs=0 fallbacks=18`.
  - **Self-test (`VCS_SELFTEST=1`):** `[selftest] 0 failure(s)`.
  - **`framerate_audit.py --steps 30,60,100 --only car,carcam,runover`:** concluído limpo (`car distance_4s`: `69.5 / 68.7 / 69.1`; `carcam half_s`: `0.467 / 0.483 / 0.46`; zero desvios novos).
  - **`scenes.py compare` (`VCS_NATIVE_COL=0` vs `1`, 8 cenas):** `8/8 ok` (`loading mean 0.03`, `flyover_road 0.01`, `gameplay_hud 0.46`, `pause_map 0.13`, `gameplay_resumed 0.49`; cenas dinâmicas com variância normal de tráfego dentro do esperado; `p1_2_col1` salvo em `build/scenes/reference`).
- **Benchmarks (`--opt lto`):**
  - **Teste manual do usuário (4K max settings):** Little Havana subiu para mínima de `64–65 fps` (`65.0 fps / 15.4 ms`, `game 13.3 ms`, `HOST 187 fps`, `105 peds / 70 vehicles`), e Downtown próximo à delegacia atingiu o teto do monitor (`75 fps`). Observado secundariamente frame pacing dente-de-serra dentro da casa do Vic (`area 2`: `74.9 fps`, `game 6.1 ms`, `p95 25.6 ms`, `max 29.0 ms`) e um bug de mira (ambos agendados após o gargalo aberto de CPU).
  - **`tools/bench.py` (60 fps, `COL=1` vs `COL=0`):** `city_575_175` `game 4.8 vs 6.0 ms`, `drive` `game 6.6 vs 7.2 ms`, `city_1325_-325` `3.1 vs 3.5 ms`, `city_-1425_-575` `4.7 vs 5.1 ms`, `fortbaxter` `5.8 vs 4.7 ms`, `city_-675_1425` `5.6 vs 5.3 ms`.
  - **`tools/bench_avenue.py --rounds 2` (4K, user save, pop cheio):** `COL=1` avg `frame 12.09 ms, game 10.86 ms, host 4.83 ms, p99 18.8 ms, >25ms 16, >33ms 5` vs `COL=0` avg `frame 11.94 ms, game 10.76 ms, host 4.71 ms, p99 22.1 ms, >25ms 52, >33ms 5` (ganho claro em tail latency `p99` e spikes `>25ms`).
  - **ABBA Redwood 4K D3D12 uncap (`p1_2_redwood_ab.py`, parado em Little Havana):** `col1 (n=1299)` `64.9 fps (avg 15.41, game 13.03, p99 29.60)` vs `col0 (n=1380)` `68.9 fps (avg 14.50, game 12.50, p99 29.08)` (`prof` `69.2 fps, game 12.36`). Em cena estacionária com tráfego aleatório, `FUN_002b8ed0` já havia sido amortizado pelo Passo 1.1 (~0% leaf no profiler), e a alocação de `float vf[32][4]` por triângulo em `vuSphereTriangleTest` (`0.7%` host no perfil) explica o empate em cena parada enquanto em movimento (`bench.py` / `bench_avenue` tail) o bypass nativo ajuda.
- **Novo diagnóstico `VCS_PROFILE=10` em Redwood 4K D3D12 uncap (`t=20..30s`, `game 12.9 ms`, `67.3 fps`):**
  - `59.3%` dos samples ainda em código guest gerado (`5685 samples`):
    - **#1 guest:** `0x0039EC18` (`3.4%` self + callees de skinning/submissão)
    - **Top guest seguintes:** `0x002F648C` (`1.1%`), `0x00141FE8` (`1.0%`), `0x0034FD60` (`1.0%`), `0x0023E74C` (`0.9%`), `0x0013A0A0` (`0.6%`), `0x002560D8` (`0.6%`), `0x00329990` (`0.6%`), `0x001933C4` (`0.6%`), `0x003814A0` (`0.5%`), `0x001F4BF0` (`0.5%`), `0x0024E8D0` (`0.5%`), `0x0032E750` (`0.5%`, `1.6%` em `t=15..20s`), `0x002BCF88` (`0.5%`), `0x00317FB8` (`0.5%`).
  - **Top host na `GameThread` / frame:** `memcpy` (`4.0%`), `o_memset` (`2.4%`), `NtGetWriteWatch` (`2.0%`), família `vcs::rslworld::*` (`~5.3%` somados: `skippable 1.5%`, `settleRegion 1.2%`, `observe 1.0%`, `itemDraw 0.6%`, `appendRegion 0.5%`, `settleIm2D 0.5%`), `matrixTimes3From` (`1.4%`), `PS2Runtime::dispatchGuestBranch` (`1.0%`), `rwFrameUpdateNative` (`0.9%`), `boxClip` (`0.9%`), `rwMatrixMulDirect` (`0.8%`), `vcs::col::vuSphereTriangleTest` (`0.7%`).
  - **Conclusão estratégica:** Atacar imediatamente `0x0039EC18` (skinning nativo / GPU skinning) e reduzir `memcpy` (`4.0%`), além dos próximos maiores blocos guest/RSL (`0x0034FD60`, `0x002F648C`, `0x00141FE8`, `0x0032E750`, `vcs::rslworld`).

### `[2026-11-26]` Passo 2.1 — Submissão/render nativo RSL/RW (`0x0039EC18` / `0x002F7D58` / `0x00317D48` / `0x001A73E0`) + `emitCfae0`/plugins (`vcs_rsl_native.cpp`)
- **Alvo:** Pipeline de submissão RenderWare/RSL no lado da `GameThread`: skinning/submissão de clumps e atomics (`FUN_0039EC18`), `clumpRender` (`0x002C0270`), `uvSet` (`0x00317D48`), `setupLights` (`0x001A73E0`) e emissor nativo `emitCfae0` (`FUN_001CFAE0`) + atalhos no-op de callbacks de plugin (`0x002F4080`, `0x002A9538`).
- **Alterado:** [src/game/vcs_rsl_render_native.h](vcs_rsl_render_native.h) (novo), [src/game/vcs_rsl_render_native.cpp](vcs_rsl_render_native.cpp) (novo), [src/game/vcs_overrides.cpp](vcs_overrides.cpp) (`vcs::rslrender::install`), [src/game/vcs_rsl_native.cpp](vcs_rsl_native.cpp) (`emitCfae0` + plugins `0x002F4080`/`0x002A9538`), [third_party/PS2Recomp/ps2xRuntime/src/lib/ps2_runtime.cpp](../../third_party/PS2Recomp/ps2xRuntime/src/lib/ps2_runtime.cpp) e [EeScheduler.cpp](../../third_party/PS2Recomp/ps2xRuntime/src/lib/Kernel/EeScheduler.cpp) (`ps2PushCheckpointDefer`/`ps2PopCheckpointDefer`/`ps2CheckpointDeferConsume`).
- **Rule 1 fallback:** `VCS_NATIVE_RSL_RENDER=0` (desativa `vcs_rsl_render_native`); `VCS_RSL_NATIVE=0` e `VCS_RSL_CFAE0=0` (desativam `vcs_rsl_native` / `emitCfae0`); `VCS_NATIVE_RSL_RENDER_VERIFY=1`, `VCS_RSL_NATIVE_VERIFY=1`, `VCS_RSL_ITEMS_VERIFY=1` validam bit a bit contra a execução guest original.
- **Armadilhas descobertas e corrigidas:**
  1. **Unwind de scheduler no meio de pacotes C++ (`[VU1 reserved lower]`):** subchamadas guest curtas chamadas de C++ nativo (`FUN_003298D8` upload BST e helpers `FUN_002F8710`/`0x00478628`/`0x0040F754`) disparavam `EeScheduler::checkpointDue` (`g_checkpointUnwind`), abortando a construção do pacote no meio das variáveis locais C++. Adicionado `CheckpointDeferGuard` + trava `t_checkpointDeferDepth` no scheduler (teto `65536` checagens, mantendo contabilização de ciclos/interrupções e `m_stopRequested` imediato).
  2. **Preservação completa de registradores salvos & arredondamento VU0:** `CallArgsBackup` passou a salvar/restaurar `$s0..$s7` e `$fp`; removido `Vu0RoundGuard` de `runVuNorm3` / `setLightDirVec` (guest `0x32E440` usa RN do host, não RZ — zerou diffs de 1-ULP).
  3. **`rpATOMICRENDER` em `clumpRenderImpl` (`FUN_002C0270`):** pular totalmente o atomic (inclusive `dispatchAtomicCallback`) quando `(rd8(rdram, atomic + 2u) & 4u) == 0u`, evitando desenhar partes ocultas/danificadas/LOD.
  4. **`uvSetImpl` (`FUN_00317D48`) e `kUvMtxInitFlag` (`0x00520F80`):** validar `kUvMtxInitFlag != 0` *antes* de escrever `envObj + 0u/4u`, pois escrever antes enganava o fallback guest de `0x00317D48` e mantinha `0x00520F80 == 0` em fallback perpétuo; roteado via `invokeUvSetHook`.
  5. **`setupLights` (`FUN_001A73E0`):** o guest chama `resetLights` (`0x001A7038`) em `!(flags & 8u)` e, em `(flags & 8u)`, aplica `setLightAmbientGlobal` apenas se `kAmbientEnableFlag != 0u` mas chama `setLightSpecGlobal` (`0x001CED88`) de `kOverrideAmbientRgb` (`0x4E5BB8`) incondicionalmente.
  6. **Paridade de estado BST & `MIPTBP1/2` (`pktBytesEqual` / `StateSnapshot`):** incluídos `renderSys1C/20`, nó raiz da árvore de upload (`0x006EE3F0`) e pool `*0x488760` (`*0x4CD58C * 0x70`) no `StateSnapshot`; `pktBytesEqual` passou a reconhecer `FLUSHE` (`0x10000000`) de `drawModel` e mascarar apenas slots de mip não inicializados no stack (`>= MXL`) nos registradores GS `0x34..0x37` (`FUN_002F62B8`).
- **Validação & Paridade (pós-limpeza de instrumentação):**
  - `p21_verify.py` (`--opt lto`, `VCS_RSL_NATIVE=0 VCS_RSL_ITEMS=0 VCS_NATIVE_RSL_RENDER=1 VCS_NATIVE_RSL_RENDER_VERIFY=1`, interior + `police` + `havana` + `autodrive`, `1415` frames): `[rsl-render-verify] checked=262144 diffs=0 fallbacks=2`, `vu1_or_stall=0`, zero crashes (`--quick` pré-limpeza atingiu `checked=327680 diffs=0 fallbacks=2`).
  - `p21_22_validate.py` (`--quick` pós-limpeza combinado Passo 2.1 + 2.2): `VCS_SELFTEST=1` -> `[selftest] 0 failure(s)`; `[rsl-native] verify: 452624 flushes the same as the original, 0 differ`; `[rsl-items] verify: 896092 draws / ops ... 0 differ; blocks: 107396 ... 0 differ`; smoke (`1161` frames) e fallback `VCS_NATIVE_RSL_RENDER=0` (`745` frames) com `crashed=False vu1_stall=0`.
  - **`tools/scenes.py compare` (`--opt lto`, 8 cenas):** `8/8 ok` (`loading 0.05`, `flyover_road 0.03`, `gameplay_hud 0.58`, `pause_map 0.13`, `gameplay_resumed 0.60`; `gameplay` `game_ms` `4.9 -> 4.1`).
- **Benchmarks (`--opt lto`):**
  - **`tools/bench.py` (`p21_on` vs `p21_off`, 60 fps cap):** `render` menor em todos os pontos — `drive` `game 5.5 + render 0.7 = 6.2 ms` vs `5.7 + 1.2 = 6.9 ms`; `city_575_175` `render 0.5 vs 0.8 ms`; `city_-675_1425` `0.4 vs 0.6 ms`; `city_-1425_-575` `0.4 vs 0.6 ms`; `fortbaxter` `0.4 vs 0.5 ms`; `city_1325_-325` `0.3 vs 0.4 ms`.
  - **`tools/bench_avenue.py --rounds 2 VCS_NATIVE_RSL_RENDER=1,0` (Passo 2.1 isolado sobre `RSL_NATIVE=1, RSL_ITEMS=1`):** `=1` média `frame 10.23 ms, game 8.79 ms, host 4.82 ms, p99 14.7 ms, >25ms 14, >33ms 12` vs `=0` `frame 10.56 ms, game 9.32 ms, host 4.73 ms, p99 15.9 ms, >25ms 20, >33ms 12` (`-0.53 ms` `game_ms`, `-0.33 ms` `frame_ms`, `-1.2 ms` `p99`, `10287/10392` vs `9993/10032` frames).
  - **ABBA Redwood 4K D3D12 uncap (`VCS_NATIVE_RSL_RENDER=1` vs `0` mantendo `RSL_NATIVE=1, RSL_ITEMS=1`):** `game_ms` caiu de `11.05` para `9.67 ms` (`-1.38 ms` na EE); como `ps2_runtime.cpp:591` contabiliza a espera pelo `RenderWorker` em `render_ms`, o tempo economizado converteu-se em espera de worker (`render_ms 1.60 -> 2.90 ms`, mantendo `~75.3–77.0 fps` no teto do D3D12 para ~`4100–4500 draws`). Contra `(0, 0, 0)` completo: `game 10.18 -> 9.27 ms` e `dma_tags 16786/17203 -> 3915/3873`.
- **Novo diagnóstico `VCS_PROFILE=10` em Redwood 4K D3D12 (`5817 samples`, `--opt lto`):**
  - Código guest recompilado caiu de `59.3%` para **`46.2%`**! (`0x0039EC18` eliminado do topo; subchamada restante `0x00329990` em `0.5%` self).
  - **Top guest inclusivos:** `0x001933c4` (`19.8%` `CWorld::Process`), `0x0021e950` (`14.8%` loop frame), `0x003ba904` (`13.0%` `CGame::Process`), `0x00109368` (`12.5%`), `0x0035d610` (`12.1%` `CRenderer`), `0x0021efb8` (`9.2%`), `0x0035cd98` (`8.7%`), `0x0035c4bc` (`6.2%`), `0x002d878c` (`5.6%`), `0x0023e74c` (`5.4%`), `0x003706c8` (`5.1%`); **top self guest:** `0x00141fe8` (`1.2%` `CAutomobile::PreRender`), `0x002560d8` (`0.8%` Script VM), `0x0033720c` (`0.7%`), `0x0023e74c`/`0x001933c4`/`0x003679d8`/`0x0034fd60`/`0x0032e750` (`0.6%`), `0x00329990` (`0.5%`).
  - **Top host na `GameThread`:** `ZwWaitForAlertByThreadId` (`10.2%`, sincronismo/worker), `memcpy` (`3.7%`), `o_memset` (`2.5%`), `NtGetWriteWatch` (`2.1%`), `atomicRenderImpl` (`1.6%`), `matrixTimes3From` (`1.5%`), `vcs::rslworld` (`skippable 1.4%`, `settleRegion 1.3%`, `observe 1.1%`, `itemDraw 0.6%`, `appendRegion 0.4%`), `rwMatrixMulDirect` (`1.0%`), `rwFrameUpdateNative` (`1.0%`), `dispatchGuestBranch` (`0.9%`), `sectorHook` (`0.9%`), `boxClip` (`0.8%`).
  - **Próximo passo:** Atacar `CRenderer` (`0x0035d610`/`0x0035cd98`/`0x0036dbf8`/`0x0036eb70`, Passo 2.2), hot paths restantes de `CAutomobile::PreRender` (`0x00141fe8`) / colisão (`0x0034fd60`) / upload BST (`0x003298d8`), e reduzir pressão de draws/worker em 4K (`4980 draws / 33 passes`, `ZwWaitForAlertByThreadId 10.2%`).
- **Passo 2.2 — `CRenderer` / `CVisibilityPlugins` / Culling Nativo (`vcs::renderer`, [vcs_renderer_native.h](file:///D:/Games/vcs-agent/src/game/vcs_renderer_native.h), [vcs_renderer_native.cpp](file:///D:/Games/vcs-agent/src/game/vcs_renderer_native.cpp)):**
  - **Escopo:** 44 hooks em `CRenderer`, `CVisibilityPlugins`, testes esfera/frustum/box e varredura de setores do mundo (`0x00241A80`, `0x0035C2C8..0x003602F8`, `0x00367528..0x00367BB0`, `0x0036D480..0x0036F0C0`, `0x003774A8`, `0x003814A0`, `0x003816F8`, `0x003C34D0`, `0x003C42C0`, `0x003C4318`), ativos por padrão em [vcs_overrides.cpp](file:///D:/Games/vcs-agent/src/game/vcs_overrides.cpp) com fallback `VCS_NATIVE_RENDERER=0` e modo dual-execution `VCS_NATIVE_RENDERER_VERIFY=1`.
  - **Paridade (`--opt lto` ThinLTO):**
    - `p22_verify.py` (`VCS_NATIVE_RENDERER_VERIFY=1` até Havana + `autodrive 40 m/s`): `[selftest] 0 failure(s)`; `p22_gp_verify` (`frame=1800`: `checked=2,510,319`, `diffs=0`); `p22_combined_verify` com `VCS_NATIVE_RSL_RENDER=1`, `VCS_RSL_NATIVE=1`, `VCS_RSL_ITEMS=1`, `VCS_RSL_WORLD=1` (`frame=1800`: `checked=2,528,528`, `diffs=0`); fallback `VCS_NATIVE_RENDERER=0` avança frames normalmente (`499 -> 619`).
    - `tools/scenes.py compare build/scenes/reference build/scenes/p22`: **`8/8 ok`** (`loading 0.03`, `pause_map 0.00`, `gameplay_hud 0.45`, `gameplay_resumed 0.64`; `packed_draws` em `flyover` `115.5 vs 115.3` e em `gameplay` `100.0 vs 101.0`).
  - **Benchmarks `--opt lto` (`VCS_NATIVE_RENDERER=1` vs `0`):**
    - `tools/bench_avenue.py` (2 rounds uncapped na avenida): frame `10.18 ms -> 9.34 ms` (**`-8.3%`**), game `8.89 ms -> 8.21 ms` (**`-7.6%`**), p99 `15.4 ms -> 13.4 ms` (**`-13.0%`**), `>25ms` `1 -> 0` (com `peds 99 vs 97`, `in view <200 m 50 vs 49`).
    - `p22_redwood_ab.py` (4K D3D12 uncap ABBA em Redwood): `80.2 fps -> 88.0 fps` (**`+9.7%`**), avg `12.47 ms -> 11.38 ms` (**`-8.8%`**), p99 `16.03 ms -> 14.69 ms` (`-8.4%`), game `9.45 ms -> 8.91 ms` (`-5.7%`); `[hdworld]` em steady-state alinhado (`drawn 454 sectors +962 extra, 202 low detail hidden`).
    - `tools/bench.py` (60Hz cap, 6 spots): `city_-1425_-575` game `5.0 -> 4.2 ms`, `city_-675_1425` `5.4 -> 5.1 ms`, `fortbaxter` `4.4 -> 4.3 ms`.
  - **Perfil `VCS_PROFILE=10` em Redwood 4K D3D12 (`5805 samples`, `--opt lto`):**
    - Código guest recompilado caiu de `46.2%` para **`45.3%`**; `CRenderer` (`0x35C000..0x370000`) caiu para apenas `0.12%` (`0x0035C4BC`) e `0.10%` (`0x0035C378`), eliminando `0x0035d610`, `0x0035cd98` e `0x003679d8` do topo.
    - **Top self guest restantes:** `0x0034fd60` (`0.9%`), `0x002560d8` (`0.9%`), `0x0023e74c` (`0.7%`), `0x00141fe8` (`0.7%`), `0x0032e750` (`0.7%`), `0x001f4bf0`/`0x0024e8d0`/`0x001f93b0` (`0.6%`), `0x001933c4`/`0x0013a0a0`/`0x0038c0e8`/`0x0011a690` (`0.5%`).
    - **Top host restantes:** `ZwWaitForAlertByThreadId` (`8.2%`), `memcpy` (`3.6%`), `NtGetWriteWatch` (`2.4%`), `o_memset` (`2.0%`), `vcs::rslworld::skippable` (`1.7%`), `atomicRenderImpl` (`1.5%`), `rwMatrixMulDirect` (`1.4%`), `matrixTimes3From` (`1.2%`), `vcs::world_hd::sectorHook` (`1.1%`), `memcmp` (`1.1%`), `dispatchGuestBranch` (`1.0%`), `vcs::rslworld::settleRegion` (`1.0%`).

### `[2026-11-26]` Paridade de render/pacing — `CRenderer` nativo, frame pacing a 75 Hz (`PS2X_PRE_PRESENT_GUARD`) e root motion sob jitter (`VCS_ANIM_STEP_FIX`)
- **Entidades e peds em cutscenes no `CRenderer` nativo (`src/game/vcs_renderer_native.cpp`):** em `renderOpaqueBucket` (`0x0035C828` / `0x0035D610`), `callWrapped(kH_RenderEntityWithAlpha)` estava restrito ao ramo `(tb == 6u || bit 49)` e o fade checava apenas `bit 49`, ocultando entidades opacas comuns (árvores, postes, portões, pickups, disquete de save) no mundo aberto; além disso, `0x0035CEBC` (`renderEverythingBarRoadsImpl`) exigia `!isPedBit` de forma redundante e mantinha `if (!s2_skip)` aninhado dentro de `if (tb == 4u)`, filtrando peds/objetos/props em cutscenes. Mover `RenderEntityWithAlpha` para fora do ramo, manter apenas `ent + 0x8C4 != 0x37` e avaliar `if (!s2_skip)` fora de `tb == 4u` restaurou a paridade visual com `pure_gs`.
- **Double-tick no main loop e uncap `VCS_FPS=0` (`third_party/PS2Recomp/ps2xRuntime/src/lib/ps2_runtime.cpp`, `src/game/vcs_framerate.cpp`):**
  - Quando `frameLead` não cabia no intervalo em `platform::waitNextFrame()` (ex.: 4K a 75 Hz / `VCS_FPS=-1`, `VCS_VSYNC=0`), o tick no topo do loop e o tick `prePresentTickOn` no fim do mesmo host frame disparavam dois `ps2HostTick` consecutivos, travando em dente-de-serra (`0.55 <-> 1.10`).
  - A trava simples `!hostTickedThisFrame` desabilitava permanentemente o `prePresentTick` em `VSync=1`/`VCS_FPS=0` após o Frame 0. A trava agora conta `unpacedStreak` apenas em frames com `prevPresentThisTick` (ignorando iterações sem `drawWindow` quando `PS2X_FRAME_STATS=0`) e exige `>= 4.0 ms` desde `topTickAt` para liberar a transição ao overlap (`PS2X_PRE_PRESENT_GUARD=0` reverte), enquanto `vcs::applyPresentRate()` passa `1000.0` Hz para `platform::setFrameRate` quando `g_cap == kCapUnlimited` (`VCS_FPS=0`) em vez de quantizar o host ao teto de `240 Hz` de `effectiveFps()`.
- **Root motion sob jitter vs props em cutscenes (`src/game/vcs_anim.cpp`):** `clumpUpdate` (`0x00154d10`) agora recebe gate via `entityUpdateAnim` (`0x003C50A8`, `CEntity::UpdateRpHAnim`), ativando `t_allowPedStepFix` exclusivamente para `CPed` (`(entity + 0x48 & 0xE) == 6u`) fora de cutscenes (`0x0048F820 + 0x1C == 0u`), substituindo `std::unordered_map` pela tabela fixa `std::array<PrevClumpDt, 2048>`. Isso preserva deslocamentos cumulativos de `CCutsceneObject` (charuto do Jerry em `ch_jerr`, Mary-Beth e mamadeira em `lou_a1`) mantendo a estabilidade de salto/calçada a `75 Hz` em Fort Baxter (`VCS_ANIM_STEP_FIX=0` desativa).
- **Profundidade de `$sp` no pipeline RW/RSL nativo (`src/game/vcs_rsl_render_native.cpp`):** `GuestSpFrame` preserva o tamanho exato dos quadros de pilha MIPS em `entityRenderImpl` (`0x20`), `clumpRenderImpl` (`0x30`), `atomicRenderImpl` (`0x1E0`), `stripRenderImpl` (`0x60`), `texBindImpl` (`0x10`) e `texApplyImpl` (`0x60`, com `callWrapped(..., 0u)` nos sub-hooks diretos), alinhando `sp < g_captureSp` em `vcs::rslworld::settleIm2D` ao original e eliminando ~420 mil itens extras/`bind 2F62B8` duplicados que sobrecarregavam o `RenderWorker`.
- **Validação final `--opt lto` (`[2026-11-27]`):**
  - `VCS_SELFTEST=1`: `[selftest] 0 failure(s)`.
  - `fortbaxter_curb_75hz` (`VCS_FPS=75`): `74.9 fps` (`frame_ms=13.4`, `max_xy=0.234`, `max_vz=0.099`, `total_dxy=39.64`, `jump_dxy=12.25`).
  - `ch_jerr` (`mission 7`, `75 Hz`, `VERIFY=1`): charuto presente em `ch_jerr @22f8e`; `[renderer-verify]` `checked=4,909,931` (`diffs=0`), `[rsl-render-verify]` `checked=720,896` (`pktSame=1, tailSame=1`, `diffs=1` apenas em `$v0` residual após cull frustum).
  - `lou_a1` (`mission 55`, `75 Hz`, `VERIFY=1`): bebê Mary-Beth + mamadeira presentes; `[renderer-verify]` `checked=1,248,037` (`diffs=0`), `[rsl-render-verify]` `checked=262,144` (`diffs=0`).
  - `tools/scenes.py compare build/scenes/reference build/scenes/final`: **`8/8 ok`** (`loading 0.00`, `flyover_road 0.01`, `flyover_tower 0.00`, `pause_map 0.00`, `cutscene` `native_draws=554.0 vs 554.0`).
  - `bench_lto_final.py` (Redwood Jr. noite `01:00`, D3D12, `2560x1080`, `PS2X_FRAME_STATS=0`, `VCS_FPS=0`, ABBA): `wait_ms=0.00`, `host=609..668`; sem tráfego `on=91.2–94.3 fps` (`game=7.74–8.28 ms`, `wrk=10.37–10.87 ms`) vs `off=92.0–94.0 fps` (`game=9.16–9.39 ms`); tráfego completo `on=66.7–70.3 fps` (`game=10.10–10.52 ms`) vs `off=67.2–73.4 fps` (`game=11.32–12.52 ms`); `tools/bench.py --fps 0` (cap 60 Hz): `city_575_175` `game 5.0 -> 4.6 ms`, `city_1325_-325` `game 2.9 -> 2.5 ms`, `drive` `game 7.7 -> 6.5 ms` (`worker 7.9 -> 7.2 ms`).

### `[2026-11-27]` Gargalo `RenderWorker` / `GsStage` em Little Havana à noite & paridade do salto (`VCS_JUMP_FIX`)
- **Arquivos adicionados/alterados:** [third_party/PS2Recomp/ps2xRuntime/include/runtime/gs/gs_upload_cache.h](../third_party/PS2Recomp/ps2xRuntime/include/runtime/gs/gs_upload_cache.h) (novo), [third_party/PS2Recomp/ps2xRuntime/src/lib/ps2_vif1_interpreter.cpp](../third_party/PS2Recomp/ps2xRuntime/src/lib/ps2_vif1_interpreter.cpp), [third_party/PS2Recomp/ps2xRuntime/src/lib/gs/gs_frontend.cpp](../third_party/PS2Recomp/ps2xRuntime/src/lib/gs/gs_frontend.cpp), [third_party/PS2Recomp/ps2xRuntime/src/lib/gs/gs_hw.cpp](../third_party/PS2Recomp/ps2xRuntime/src/lib/gs/gs_hw.cpp), [src/game/vcs_hle_world.cpp](../src/game/vcs_hle_world.cpp), [src/game/vcs_rsl_world.cpp](../src/game/vcs_rsl_world.cpp), [src/game/vcs_physics_timing.h](../src/game/vcs_physics_timing.h), [src/game/vcs_physics_timing.cpp](../src/game/vcs_physics_timing.cpp), [src/game/vcs_overrides.cpp](../src/game/vcs_overrides.cpp), [docs/PERFORMANCE.md](PERFORMANCE.md), [docs/PS2RECOMP_PATCHES.md](PS2RECOMP_PATCHES.md), [docs/game-internals.md](game-internals.md).
- **Gargalo eliminado (`RenderWorker` + `GsStage` em `worker_ms = 14.5–15.8 ms` -> `6.9–11.0 ms`):**
  1. `sendKicksBatched` ([src/game/vcs_hle_world.cpp](../src/game/vcs_hle_world.cpp), `PS2X_CLIP_BATCH=0`): junta kicks consecutivos recortados em C++ (`PRIM=3`/`5`/`4`) num único pacote `GS_PRIM_TRIANGLE` por draw (até `32766` vértices), cortando ~62 mil pacotes/quadro para ~1.200 lotes.
  2. Cache SPSC 4-way em lockstep ([gs_upload_cache.h](../third_party/PS2Recomp/ps2xRuntime/include/runtime/gs/gs_upload_cache.h), `ps2_vif1_interpreter.cpp`, `gs_frontend.cpp`, `vcs_rsl_world.cpp` `Op::directHash`; `PS2X_HW_UPLOAD_CACHE=0`, `PS2X_HW_NATIVE_CACHE=0`): `8192` slots GIF e `16384` slots Native com pinning zero-copy em `PendingUpload` (`deferPinned 1,386,115 deferCopied 0`), eliminando cópias no ring buffer e hashing redundante sem tocar em `ps2_runtime.h`.
  3. `gs_hw.cpp`: `recordPacked` habilitado em widescreen `fbw == 10u` quando `!anyFar` (`PS2X_HW_PACKED_3D_WIDE=0`), `PendingDeque<PendingUpload>` circular, `m_uploadAtFlat` (`16384` blocos), `m_rectBlocksMemo` (`128` entradas), `m_soleBlockWriter` e `NativeGroup::parts` preguiçoso (`materializeGroupParts`, `onEvictPinnedNativeSlot`).
- **Distância e velocidade do salto (`VCS_JUMP_FIX=0` em `vcs_physics_timing.cpp`):**
  - `kPlayerAirStuckPush` (`0x00233114` via `FUN_00232de8`) agora compara `ped + 0x108` contra `g_prevFrameStep` (evitando impulso `3.0` não-escalado no ar sob variação de frametime) e `kJumpTakeoffImpulse` (`0x002128C4`) compensa o Euler semi-implícito usando `g_avgStep2 / g_avgStep1` atualizado por `onPhysicsFrame` em `markFrameKick`.
  - Medido em `diag_jump.py` no binário final ThinLTO (`orig30` / `real75` / `real0_mod`): `stand_jump` `2.80 m (561 ms)` / `2.77 m (560 ms)` / `2.78 m (560 ms)`; `run_jump` `4.74 m` / `4.69 m` / `4.69 m`; `sprint_jump` `6.10 m` / `6.05 m` / `6.04 m`; `v9_curb_nojump` `0.00 m`; `v10_slope_jump` `8.89 m` / `8.99 m` / `8.98 m`.
- **Benchmarks e Paridade `--opt lto` (ThinLTO final):**
  - Little Havana (`00:30` noite, `PCJ600`, configs máximas + `VCS_HD_RADIUS=3` + `VCS_ASPECT=219`), `VSYNC=0`: `Verdi` **`119.3 fps`** (`frame_ms=8.4`, `worker_ms=6.9`, `gs_ms=7.9`), `LittleHavana_Hotel` **`87.4 fps`** (`frame_ms=11.4`, `worker_ms=11.0`), `LittleHavana_Ave` **`93.5 fps`** (`frame_ms=10.7`, `worker_ms=10.7`, `gs_ms=9.7`); `VSYNC=1` (75 Hz): **`74.9 fps`** cravados nos 3 pontos (`Verdi wait_ms=4.9`, `Hotel wait_ms=4.6`, `Ave wait_ms=4.2`).
  - Paridade e checks finais: `PS2X_HW_PACKED_VERIFY=1` (`320,000+` draws, `0 differ`), `PS2X_HW_CLUT_VERIFY=1` (`400,000+` checagens, `0 differ`), `PS2X_HW_DECODE_VERIFY=1` (`0` diffs), `VCS_SELFTEST=1` (`[selftest] 0 failure(s)`), `scenes.py compare` `8/8 ok`, `cutscene_check.py` `PASS` (`sky band black -> 152`, `max step 18`).
