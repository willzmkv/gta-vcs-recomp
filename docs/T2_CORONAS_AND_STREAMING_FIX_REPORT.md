# Relatório Técnico: Correção de Coronas e Streaming (T2)

> **Data:** 05 de Novembro de 2026  
> **Escopo:** `src/game/vcs_stream.cpp`, `src/game/vcs_distance.cpp`, `src/game/vcs_distant.cpp`, `docs/T2_POPULATION_AND_DISTANT_LIFE_PLAN.md`  
> **Status:** Resolvido e validado com sucesso (`distance_check.py: PASS`)

---

## 1. Contexto e Problemas Relatados

1. **Travamento no Boot / Jogo Não Carrega**:
   - Uma tentativa prévia de implementar streaming multithread e acelerar chunks modificou `src/game/vcs_stream.cpp`, adicionando hooks para `kStreamPump` (`0x00220FA8`), `kProcessLoadingChannel` (`0x003C7048`) e `kStreamEnqueue` (`0x002213D0`).
   - O jogo compilava, mas congelava durante a inicialização/loading, travando o thread principal e encerrando com:
     ```text
     [guest-branch:missing-target] kind=DirectJump op=EE scheduler source=0x1fdabb0 target=0x1fdabb0 pc=0x1fdabb0 ra=0x1fdabb0 ...
     [ee] thread 1 -> dormant pc=0x01fdabb0 ra=0x01fdabb0 sp=0x01fff3f0
     ```
2. **Estouro de Coronas (Brilho e Tamanho Excessivo ao se Aproximar)**:
   - Luzes de postes e veículos tinham um aumento desproporcional de raio na tela quando o jogador passava perto ou olhava em ângulos laterais no modo widescreen (16:9), estourando o brilho da tela.

---

## 2. Diagnóstico e Causa Raiz

### 2.1. O Modelo de Execução do PS2Recomp vs. Loops C++
- No **PS2Recomp**, o código MIPS recompilado não se comporta como funções C++ normais que executam do início ao fim sincronicamente se houver chamadas internas `JAL` para outras funções guest.
- Em `generated/ee/FUN_003c7048_0x3c7048.cpp` e `generated/ee/FUN_00220fa8_0x220fa8.cpp`:
  ```cpp
  if (!runtime->dispatchGuestBranch(rdram, ctx, 0x3C66B0u, 0x3C70BCu, 0x3C70C4u, PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
      return;
  }
  ```
- Quando `dispatchGuestBranch` é executado, a função retorna imediatamente ao scheduler com o `$sp` já decrementado para alocar o stack frame.
- Ao colocar `g_processLoadingChannel()` ou `g_streamPump()` dentro de um loop `for (int pass = 1; pass < 6; ++pass)` em C++:
  - O loop chama a função novamente antes que a execução anterior tenha concluído e desempilhado o stack.
  - A pilha é decrementada repetidamente, corrompendo os registradores salvos (`$s0..$s7`, `$ra`).
  - Ao término, o registrador de retorno `$ra` continha lixo (`0x1fdabb0`), fazendo o despachante saltar para um endereço inválido e desativar o thread (`[ee] thread 1 -> dormant`).

### 2.2. Concorrência Desordenada no Canal de Streaming
- O protocolo de CDVD/streaming do VCS opera com canais sequenciais (0 a 5).
- Criar múltiplas threads de worker consumindo da mesma fila sem agrupamento e ordenação por canal fazia requisições posteriores terminarem antes de requisições anteriores.
- `streamPoll` retornava status concluído precocemente, levando o motor a ler texturas e modelos parcialmente gravados na memória RAM guest.

### 2.3. Matemática da Projeção de Coronas em Widescreen
- O motor (`CSprite::CalcScreenCoors`) projeta o tamanho na tela da corona dividindo o raio pelo depth focal da câmera:
  $$\text{raio\_tela} = \frac{\text{raio\_mundo} \times \text{projecao}}{z_{\text{cam}}}$$
- Quando o jogador se aproxima de um poste na lateral da calçada, $z_{\text{cam}}$ (produto escalar com o vetor de visão da câmera `dot`) tende a zero, mesmo que a distância euclidiana seja de 5 a 15 metros.
- Isso fazia o fator $\frac{1}{z_{\text{cam}}}$ explodir em 20x a 50x, preenchendo a tela inteira.
- Adicionalmente, forçar `normalAngle = 0.85` acionava a fórmula de glare direcional (`6.0 * (1 - normalAngle * 2/pi)^8 + 0.5`) em luzes omnidirecionais (postes comuns).

---

## 3. Correções Aplicadas

### 3.1. Restauração do Streaming Seguro (`src/game/vcs_stream.cpp`)
- Removidos os hooks instáveis `kStreamEnqueue`, `kStreamPump` e `kProcessLoadingChannel`.
- Restabelecida a execução determinística em fila única sequencial, garantindo que nenhum asset seja lido antes de estar completamente gravado em RDRAM.

### 3.2. Limpeza em `src/game/vcs_distance.cpp`
- Removidos os wrappers `scanSectorList` e `shouldModelBeStreamed`, evitando interferências no mecanismo de prioridade de colisão do jogo (`ms_bStreamingLocked`).

### 3.3. Correção Geométrica das Coronas (`src/game/vcs_distant.cpp`)
- Removido o hook instável `coronasRender` (`0x0027C418`), que sofria do mesmo problema de yielding do recompilador.
- Toda a lógica de atenuação foi centralizada no momento de registro da corona (`CCoronas::RegisterCorona` - `0x0027DD80`):
  - Limite do raio base para valores realistas (`0.65m` para carros, `0.85m` para postes).
  - Multiplicação contínua do raio (`$f12`) e do canal alfa (`$t0`) por um fator de aproximação suave:
    ```cpp
    if (dot < 9.0f || dist < 20.0f)
    {
        const float fade = std::clamp((dot - 1.5f) / 7.5f, 0.0f, 1.0f) *
                           std::clamp((dist - 2.0f) / 18.0f, 0.0f, 1.0f);
        const float scaledSize = clampedBaseSize * std::max(0.04f, fade);
        ctx->f[12] = std::copysign(scaledSize, ctx->f[12]);
        const uint32_t origAlpha = GPR_U32(ctx, 8) & 0xFFu;
        const uint32_t newAlpha = static_cast<uint32_t>(origAlpha * fade);
        SET_GPR_U32(ctx, 8, newAlpha);
    }
    else
    {
        ctx->f[12] = std::copysign(clampedBaseSize, ctx->f[12]);
    }
    ```
  - Como o raio decresce linearmente com `dot`, o termo $\frac{\text{fade}}{z_{\text{cam}}}$ estabiliza em uma constante assintótica finita quando o jogador se aproxima, e a corona esmaece naturalmente a menos de 2 metros sem nenhum pico de brilho.

### 3.4. Atualização da Documentação de Arquitetura (`docs/T2_POPULATION_AND_DISTANT_LIFE_PLAN.md`)
- Incluída a seção **7. Critical Learnings & Anti-Patterns (Post-Mortem: 2026-11-05)** documentando as regras para evitar novas regressões por futuros agentes.

---

## 4. Validação e Testes

### 4.1. Compilação
- `python tools/build.py --quick`: compilação e linkedição concluídas em 11s sem warnings ou erros, gerando `build\vcs.exe`.

### 4.2. Estabilidade em Execução
- Teste de 15 segundos com `python tools/run.py 15 test_fixed`:
  - Jogo carregou o save normal da bancada (`BASLUS-21590GTA40001`).
  - Carregou mais de **325 texturas** (PNG/DDS).
  - Processou mais de **1.202.861 pacotes de mundo HLE** (mais de 58 milhões de vértices renderizados).
  - Encerrou normalmente sem nenhuma falha de segmentação ou `missing-target`.

### 4.3. Teste Automatizado de Distância (`tools/distance_check.py`)
Execução completa com verificação de entidades visíveis:
```text
original     100%: scale 1.000 lod 1.000 gen 1.000, 1 visible entities  -> ok
view28       352%: scale 3.520 lod 3.520 gen 1.000, 19 visible entities -> ok
back         100%: scale 1.000 lod 1.000 gen 1.000, 1 visible entities  -> ok
visible entities grow with the distance: yes
PASS - D:\Games\VCS Recomp\build\distance_check\20261105-124148
```
Resultado: **PASS**.
