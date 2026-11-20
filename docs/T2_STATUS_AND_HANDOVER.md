# Status da Task T2: Pop-in de Props / Antenas e Handover para o Próximo Agente

Data: 2026-11-06  
Branch: `task/T2-beta`  
Commit Atual: `c46c3c3` (`fix(streaming): remove unstable FUN_0035DEA8 hook to resolve loading screen hang`)

---

## 1. Resumo Executivo & Status Atual

1. **Tela "Carregando..." (Resolvido)**:
   - O freeze infinito relatado pelo usuário ao iniciar o jogo foi **completamente eliminado**.
   - O binário `build\vcs.exe` foi recompilado com sucesso via `--quick`.
   - Um teste de execução de 5 segundos (`tools/run.py 5 test_quick --hidden`) confirmou que o save `/BASLUS-21590GTA40001/1 Cholo Vitorioso` foi lido, a DMA foi disparada e o loop do jogo iniciou normalmente.

2. **Desempenho / Slowdown**:
   - O usuário confirmou previamente melhora de **100% no desempenho**. Os orçamentos de peds/tráfego (`updateBudget` / `trimCrowd`) e o HLE de renderização nativo continuam funcionando com alta estabilidade e taxa de quadros elevada.

3. **Problema Pendente para o Próximo Agente**:
   - O usuário reportou:
     > *"Arbustos, placas de sinalização, antenas, placas de prédios ainda carregam muito perto. Restante, perfeito."*
   - O restante do mundo, edifícios principais, ruas e veículos já carregam em longa distância. Faltam apenas estes elementos específicos de detalhe urbano e vegetação baixa.

---

## 2. Diagnóstico Técnico Detalhado

### 2.1 Por que o hook em `FUN_0035DEA8` travou o jogo? (Nunca mais reabilitar)
- `FUN_0035DEA8` foi tentado como forma de estender a varredura do triângulo do frustum.
- Porém, na descompilação (`generated/ee/FUN_0035dea8_0x35dea8.cpp`):
  1. A função contém **10 chamadas diretas a `runtime->dispatchGuestBranch`**, que retornam ao scheduler do EE. Chamar `g_func35DEA8(rdram, ctx, runtime)` dentro de uma função C++ viola a Regra 2 de `AGENTS.md`.
  2. O primeiro parâmetro `$a0` passado por `FUN_0035e620` é um ponteiro para uma struct de heap persistente do renderizador (`$s0`), e não um array temporário de vértices. Ao sobrescrever `vertAddr + 8u`, `+ 12u`, etc., o estado do renderizador foi corrompido, causando travamento permanente no `WaitSema` durante a tela de "Carregando...".
  3. **Conclusão**: O hook foi desativado no commit `c46c3c3`. `FUN_0035DEA8` **NÃO DEVE** ser substituído via `replaceFunction`.

### 2.2 Por que Arbustos, Placas, Antenas e Letreiros dão pop-in aos 30 metros?
Investigando `generated/ee/FUN_0035fae8_0x35fae8.cpp` (`ScanSectorList`) e `FUN_0035efd8_0x35efd8.cpp` (`SetupEntityVisibility`):

1. **Flags dos Modelos (.IDE)**:
   - Antenas de telhado, caixas d'água, ar condicionado e letreiros de prédios possuem flag `0x2000` (attached clutter / sub-objects).
   - Placas de rua, postes quebráveis e arbustos possuem flag `0x4000` (breakables / small foliage).
   - Na `.IDE`, o alcance base (`DrawDist`) desses objetos é propositalmente minúsculo no PS2 original (20m a 40m).

2. **O Retorno de `SetupEntityVisibility` (`0x0035EFD8`)**:
   - `SetupEntityVisibility` chama `GetAtomicFromDistance` (`0x003A52F8`) e `GetAtomicFromDistanceWithFlags` (`0x003A53E0`).
   - Se a distância da câmera até a entidade for maior que o LOD do modelo, ambas retornam `0` (nulo), e `SetupEntityVisibility` retorna `0` (invisível).

3. **O "Fallback de 30m" em `ScanSectorList` (`FUN_0035fae8`, linhas 585–620)**:
   - Quando `SetupEntityVisibility` retorna `0`, `ScanSectorList` não descarta imediatamente se o modelo tiver as flags `0x2000` ou `0x4000`.
   - Ele faz uma checagem hardcoded em ponto flutuante:
     ```cpp
     // 0x35fdf0: -30.0f, 0x35fe18: +30.0f
     if (abs(camera.x - entity.x) < 30.0f && abs(camera.y - entity.y) < 30.0f) {
         // Adiciona na Lista 4 (0x746A00, limite de 149 objetos)
     } else {
         goto label_35fe88; // Descarta completamente! Não renderiza e não faz stream!
     }
     ```
   - **Esse é o exato motivo do pop-in repentino aos 30 metros!** O jogo descarta esses objetos na varredura do setor assim que a distância passa de 30m porque `SetupEntityVisibility` os considerou invisíveis!

---

## 3. Solução Arquitetural Recomendada para o Próximo Agente

Para que esses objetos renderizem a 300m–800m sem cair no descarte de 30m, **`SetupEntityVisibility` deve considerá-los visíveis** (retornando `1` para lista normal de render ou `3` para streaming).

### Onde atuar:
Em `src/game/vcs_distance.cpp`:

1. **`atomicFromDistance` (`0x003A52F8`)**:
   - Inspecionar a lógica de seleção de atômicos:
     ```cpp
     uint16_t flags;
     std::memcpy(&flags, rdram + ((mi + 0x3Au) & PS2_RAM_MASK), 2);
     const int count = rdram[(mi + 0x38u) & PS2_RAM_MASK];
     ```
   - Para modelos com flags `flags & 0x6000` (ou `baseLod <= 60.0f`), garantir que quando `viewFactor > 1.0f`, o `limit` de distância do atômico retornado seja estendido até `propFloor` (ex.: `std::clamp(300.0f * viewFactor, 300.0f, 900.0f)`).
   - Certificar-se de que o atômico retornado seja válido (`atomicPtr != 0`).

2. **`func3A53E0` (`0x003A53E0`)**:
   - `SetupEntityVisibility` chama `func3A53E0` com `distance - 20.0f` para verificar se o objeto está na faixa de alpha-fade.
   - Garantir que `func3A53E0` também retorne o atômico válido para distâncias estendidas até `propFloor`.

3. **`largestLodDistance` (`0x003A5380`)**:
   - Já possui o bypass do clamp de 330m (`kSetupEntityVisibility330ClampReturn`).
   - Assegurar que nos pontos de prefetch (`kSetupEntityVisibilityPrefetchNoFadeReturn`, etc.), o retorno de `largestLodDistance` seja suficientemente grande para que o CStreaming carregue os modelos com antecedência.

4. **`VisibilityInit` (`0x0036EBF8`)**:
   - Verificar os valores de cutoff calculados em `0x004CD5C0u` e afins. Na inicialização do jogo, a fórmula usa `own * (own * 400.0f)`. Garantir que esses limites acompanhem o `viewFactor`.

---

## 4. Instruções de Verificação

1. Para compilar rapidamente após alterações:
   ```powershell
   python tools/build.py --quick
   ```
2. Para testar se o jogo inicia sem travar:
   ```powershell
   python tools/run.py 10 test_run --hidden
   ```
3. Para validar visualmente no jogo real:
   - Iniciar o jogo normalmente através do launcher ou `run.py`.
   - Carregar o save do usuário ("Cholo Vitorioso").
   - Caminhar ou dirigir pelas ruas e observar arbustos nas calçadas, placas de trânsito e antenas nos topos dos prédios.
