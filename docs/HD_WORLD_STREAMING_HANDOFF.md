# Handoff: distância detalhada do mundo e streaming de props

Data: 2026-11-09. Este documento é a instrução operacional para retomar o
trabalho de aumentar a distância de detalhe do mundo de GTA: Vice City Stories.
Ele descreve o que foi confirmado, o que não pode ser usado, e a ordem segura
de investigação/implementação.

## Objetivo correto

O objetivo não é só estender o LOD dos modelos. Em VCS existem dois limites
independentes:

1. `src/game/vcs_distance.cpp` altera a distância em que uma entidade já
   residente escolhe seu atomic/LOD. Isso trata prédios, objetos e props que o
   motor já conhece e cujos modelos já foram requisitados.
2. A geometria detalhada da cidade pertence a uma pequena janela de áreas
   carregadas em torno da câmera. Fora dessa janela o jogo desenha a cidade de
   baixo detalhe. Props/setores de uma célula não residente não podem ser
   recuperados apenas elevando a distância do LOD.

Uma solução completa precisa respeitar os dois níveis: manter os modelos dos
props requisitados antes que entrem na vista e aumentar de forma real a janela
de células detalhadas, ou desenhar células adicionais pelo caminho nativo RSL.

## Correção de terminologia

O usuário descreveu blocos de 50 x 50. A unidade efetiva confirmada no jogo é
uma grade hexagonal de células de aproximadamente **125 x 108,25 unidades**.
Uma área detalhada contém uma janela de 3 x 3 células. A previsão normal usa
`câmera + frente * 50 + velocidade do jogador * 150`; portanto 50 é parte do
deslocamento de previsão, não a dimensão da célula.

## Estado do repositório ao fazer este handoff

- Branch: `task/T2-beta`.
- HEAD antes deste handoff: `ff8301f` (`docs(hd-world): record safe streaming
  extension design`).
- As alterações locais abaixo já existiam e **não foram criadas nem validadas
  por este handoff**:
  - `src/game/vcs_distance.cpp`
  - `src/game/vcs_distant.cpp`
  - `src/game/vcs_hud.cpp`, `src/game/vcs_hud.h`
  - `src/game/vcs_world_hd.cpp`
  - `src/main.cpp`
- Não faça `reset`, `checkout` ou limpeza dessas alterações sem acordo com o
  usuário. Trate-as como uma tentativa experimental a ser preservada para
  comparação, não como base confiável.
- Há worktrees preexistentes em `.claude/worktrees/`, inclusive
  `hd-world-further`. Antes de editar código, crie uma nova worktree isolada ou
  obtenha orientação do usuário sobre qual cópia deve ser alterada.

## Diagnóstico: causas prováveis e severidade

### P0 — áreas falsas sobre campos do objeto de nível

`src/game/vcs_world_hd.cpp::initHDAreaSlots()` preenche `g_hdAreaSlots` com
`level + 0x25c`, `+0x260`, ..., `+0x288` e os trata como doze estruturas de
área. Isto é incorreto.

As áreas reais são quatro estruturas de `0xa0` bytes iniciando em `level +
0x2c`. Já `+0x248`, `+0x250`, `+0x258` são ponteiros para áreas ativas e
`+0x24c`, `+0x254`, `+0x25c` são os pares associados às áreas. `+0x260` é o
temporizador de cross-fade; os campos seguintes são estado da fila/IO. Ler ou
escrever `area + 0x18`, `area + 0x20` e `area + 0x24` nesses endereços corrompe
o objeto `level`.

Consequência: carregamentos falsos, listas inválidas e crashes dependentes de
tempo.

### P0 — código guest chamado de thread host

`processHdRequestQueue()` cria um `R5900Context` sintético e chama
`FUN_002415B0` (`RequestArea`) em um `std::thread` host. Isso viola duas regras
do runtime:

- a RAM guest, a fila de streaming e o scheduler são acessados simultaneamente
  pelo thread do jogo e pelo worker;
- funções recompiled podem retornar ao scheduler guest no meio do corpo; uma
  chamada host direta não significa “executar a função até o fim”.

Mesmo que uma execução curta pareça funcionar, a concorrência com callbacks de
leitura e a ausência do contexto guest válido tornam esse caminho inválido.

Consequência: corrupção/race e hangs/crashes intermitentes.

### P0 — splice de listas durante o draw

`worldDrawHook()` anexa a lista de setores de cada “área HD” ao tail da lista
atual, usando `+0x4c` como próximo nó. O código não prova que esta é uma lista
linear desse tipo, não faz deduplicação por id, não limita a travessia contra
ciclos e restaura apenas a cabeça depois de alterar nós pertencentes a outras
áreas. A tentativa de mesclar big buildings está estruturalmente incompleta.

Consequência: ciclos, listas duplicadas, listas persistentes após o frame e
acesso a geometria cujo buffer pode ser reutilizado.

### P1 — a implementação atual não satisfaz a carga de props

Mesmo se os três problemas anteriores não existissem, anexar listas no draw não
garante que `ScanSectorList` tenha executado a requisição de modelos para cada
prop. O caminho de streaming de modelos precisa ser observado explicitamente;
um atomic de alto detalhe só é selecionável se seu modelo estiver residente.

### P1 — hooks de LOD precisam permanecer separados da arquitetura de áreas

Em `vcs_distance.cpp`, `GetAtomicFromDistance` recebeu pisos artificiais para
props pequenos e hooks relacionados a `ms_bStreamingLocked`. Isso pode melhorar
o pop-in de modelos já requisitados, mas não substitui a janela de áreas. Não
acople mais patches destrutivos de flags/listas a essa lógica até medir as duas
coisas independentemente.

## Fatos confirmados sobre o binário (fontes e endereços)

Fonte primária: `docs/game-internals.md`, seções 9f e 9h; a análise original
foi feita com Ghidra e dumps da RAM.

| Item | Endereço / layout | Fato confirmado |
|---|---|---|
| Ponteiro do nível | `*(0x0048F4F8)` | Durante um jogo normal aponta para o objeto de nível. |
| Slots reais | `level + 0x2c`, quatro x `0xa0` bytes | Toda a janela detalhada usa estes quatro slots, não campos depois de `+0x248`. |
| Áreas ativas | `+0x248`, `+0x250`, `+0x258` | Ponteiros para área atual, próxima e outra área/cross-fade. |
| Pares de área | `+0x24c`, `+0x254`, `+0x25c` | Campos associados, usados por peças de área; não são slots extras. |
| Escolha | `FUN_0023C820` | Executada por frame; prevê uma posição à frente da câmera. |
| Área por posição | `FUN_0023C188` | Converte posição na célula/área correspondente. |
| Requisição | `FUN_002415B0(level, x, y)` | Enfileira uma área no mecanismo original; requer contexto do jogo. |
| Leitura | `FUN_002417D0` | Inicia leitura assíncrona de peça, prioridade 500. |
| Conclusão | `FUN_00241278` | Reloca a peça e parte para a próxima pendência. |
| Troca | `FUN_0023D740` | Promove a próxima área conforme a câmera se aproxima. |
| Draw mundo | `FUN_0023E748` | Mescla listas da célula corrente e anterior apenas durante cross-fade. |
| Draw setor | `FUN_002409C8`, `FUN_00240ED8` | Faz frustum/culling e desenha setores que estão nos slots reais. |
| Buffer de áreas | `0x00CB7A80..0x0112A660` | ~4,5 MB compartilhados pelas peças das áreas; pertence ao heap principal. |
| Fila de peças | `level + 0x294`, contador `+0x414` | Fila nativa que a leitura/callback consome. |

Uma lista de setores contém o mapa inteiro, mas seus buckets distinguem o mapa
de baixo detalhe da geometria detalhada. Setores detalhados próximos têm owner
tag de célula/área e os de baixa qualidade são escondidos onde há substituição.
O draw já sabe mover a origem de cada célula e fazer cross-fade; uma extensão
segura precisa reutilizar ou reproduzir essas invariantes, não concatenar
ponteiros às cegas.

## Procedimento de Ghidra obrigatório

O projeto analisado já existe em `ghidra/project/vcs.gpr`. Não reimporte o ELF
nem sobrescreva o projeto; abra-o em read-only para decompilar funções.

1. Confirme que `build/elf/SLUS_215.90` é o ELF que corresponde a esta cópia.
2. Configure Ghidra 12.1.4 e um JDK 21+ válido. No ambiente atual, o caminho
   de Ghidra é:

   ```powershell
   $env:GHIDRA_HOME = 'D:\ghidra_12.1.4_PUBLIC_20260921\ghidra_12.1.4_PUBLIC'
   $env:JAVA_HOME = '<diretório de um JDK 21+>'
   ```

3. Execute a decompilação read-only, salvando somente em `build/analysis/`:

   ```powershell
   python tools/eedecomp.py 0x23c820 0x2415b0 0x2417d0 0x241278 0x23d740 0x23e748 0x2409c8 0x240ed8 --out build/analysis/decomp/hd_world_functions.c
   ```

4. Para cada função, registre no `docs/game-internals.md`: argumentos,
   offsets lidos/escritos, chamadores, estado da fila, e se pode ceder ao
   scheduler. Não declare um offset como slot livre por proximidade.
5. Faça uma tabela de transição de estados para cada um dos quatro slots:
   livre -> requisitado -> lendo peça N -> completo -> próximo/atual ->
   cross-fade -> reutilizável. Inclua quem pode mudar cada estado.
6. Compare a lógica de merge de `FUN_0023E748` no cross-fade com a tentativa
   host. Só será aceitável desenhar mais de uma área quando os owner tags,
   matrizes/origens, ocultação de LOD baixo, e a vida do buffer forem todos
   compreendidos.

Nota do ambiente atual: a execução headless falhou antes de abrir o projeto
porque o sandbox impediu Ghidra de gravar `java_home.save` sob `%APPDATA%` e
porque não havia `JAVA_HOME` configurado. Isso não é uma conclusão sobre o
binário. Em uma sessão com permissão normal, configure o JDK e repita o comando
acima; não reinstale Ghidra sem necessidade.

## Plano obrigatório de implementação

### Fase 0 — tornar a cópia testável antes de qualquer feature

1. Crie worktree/branch nova. Não edite a cópia suja diretamente.
2. Faça build rápido e registre se a árvore experimental atual compila:

   ```powershell
   python tools/build.py --quick
   ```

3. Rode uma baseline com `VCS_HD_RADIUS=0`; use diretório de dados de teste,
   nunca o card do usuário:

   ```powershell
   python tools/run.py 60 hd_baseline --hidden
   ```

4. Rode o mesmo percurso/teste com cada recurso isolado: LOD alto sem HD world;
   HD world sem pisos de LOD; ambos juntos. Arquive logs em
   `build/<ferramenta>/<nome>`.

### Fase 1 — remover/desativar as corrupções, mantendo fallback original

1. Escreva primeiro um teste de fonte/integração que falhe quando
   `vcs_world_hd.cpp` contiver `std::thread`, uma chamada direta de
   `lookupFunction(kRequestArea)` a partir de worker, ou uso de
   `level + 0x25C` como slot de área. Um pequeno verificador Python em
   `tools/` é suficiente e deve retornar não-zero com mensagens claras.
2. Rode o teste e observe a falha na cópia atual.
3. Remova o worker host, a fila host e o splice de listas. Enquanto não houver
   um modelo de slots comprovado, `world.hd_radius > 0` deve informar no log e
   voltar ao caminho original em vez de simular uma extensão.
4. Rode o teste de novo, depois `python tools/build.py --quick` e uma execução
   de 60 s com slider 0 e máximo. O máximo pode não aumentar alcance nessa
   etapa, mas nunca pode escrever em memória guest inválida.
5. Documente a remoção e os endereços confirmados em `docs/game-internals.md`;
   registre a decisão em `docs/DECISIONS.md`.

### Fase 2 — instrumentar no thread do jogo

1. Estenda `src/game/vcs_world.cpp`, que já tem `VCS_TRACE_AREAS=1`, sem
   chamar funções guest diretamente. O hook só pode observar os argumentos e
   chamar a original imediatamente, conforme a regra do projeto para hooks.
2. Logue, com rate limit: frame, função, `level`, slot real, célula x/y, estado
   de `+0x264/+0x270/+0x280/+0x284/+0x288/+0x28c/+0x290/+0x414`, intervalo de
   buffer e cabeças de listas. Valide todos os ponteiros contra RAM guest antes
   de desreferenciar.
3. Adicione um script de teste que percorra uma rota em velocidade alta com
   `VCS_TRACE_AREAS=1`, reduza o log a uma sequência por transição de estado e
   falhe se aparecerem estados impossíveis, endereços fora da RAM, watchdog,
   `[guest-branch:unlisted-resume]` ou timeout.
4. Execute pelo menos três vezes com o mesmo save de cópia e compare as
   transições. Só prossiga quando forem determinísticas.

### Fase 3 — decidir a estratégia com evidência

Escolha somente depois da Fase 2:

**Opção A: janela guest estendida.** Só é permitida se a decompilação provar
como aumentar slots, buffers, owner tags, fila e os limites das rotinas sem
reutilizar memória de outra área. Todas as requisições devem ser enfileiradas
pelo thread/scheduler guest, não por um worker host. Requer testes de troca,
teleporte, menus, save/load, missão e soak.

**Opção B: pré-busca da janela normal.** Se o motor tiver exatamente quatro
slots fixos, ajuste apenas a previsão normal para priorizar a direção de
movimento. Isso melhora o pop-in à frente, mas deve continuar descrito como
“preload ahead”, não como maior raio simultâneo. Não anuncie que resolve o
objetivo completo.

**Opção C: células extras pelo RSL nativo.** Se slots/buffers forem rígidos,
   implemente um lifecycle host separado: índice da célula, leitura, decode,
   recursos, owner tag/transform e submissão. A célula guest original continua
   sendo a referência. Este é o único caminho que aumenta de fato o raio sem
   invadir estruturas guest fixas, mas é uma tarefa maior e deve ter spec
   própria antes da implementação.

### Fase 4 — props e LOD, após a estratégia de células

1. Instrumente `ScanSectorList`, `ScanBigBuildingList` e as chamadas de
   `CStreaming::RequestModelFlags`: por prop/modelo, registre distância,
   requisição, chegada ao streaming e atomic escolhido.
2. Crie uma cena fixa com props pequenos (postes, placas, vegetação e antenas)
   em distâncias conhecidas. Use câmera livre apenas para observação; a origem
   de streaming continua sendo a câmera real do jogo.
3. Compare quatro configurações: original, apenas view/detail, apenas células
   extras/pré-busca, ambos. O sucesso é a sequência “setor residente -> modelo
   requisitado -> modelo carregado -> atomic alto selecionado”; não apenas uma
   screenshot bonita.
4. Não altere flags permanentes de `CBaseModelInfo` ou entidades globalmente
   sem provar a propriedade e restaurar o estado. Flags de fade/folhagem podem
   ser compartilhadas por múltiplas instâncias e um write global pode alterar
   comportamento fora do raio testado.

## Critérios de aceite e regressão

- `world.hd_radius=0` é indistinguível do comportamento original.
- Não há `std::thread` host entrando em código guest, nem splice de listas
  guest em hooks de draw.
- Toda expansão de raio possui evidência de buffers/slots reais ou lifecycle
  nativo próprio.
- O slider de visão/detalhe não mostra prop alto detalhe apenas por sorte:
  logs demonstram solicitação e residência antecipada do modelo.
- Execute no mínimo:

  ```powershell
  python tools/build.py --quick
  python tools/distance_check.py build/distance_check_hd
  python tools/cruise.py run build/cruise/hd_world
  python tools/soak.py build/soak/hd_world --hours 3
  ```

- Use `VCS_DATA_DIR` apontando para uma cópia de card para qualquer teste que
  possa gravar. Nunca use `%LOCALAPPDATA%/VCSRecomp/mc0`.
- Registre números de `gamectl.py perf` antes/depois, screenshots comparáveis e
  conclusão em `docs/PERFORMANCE.md`, `docs/PROGRESS.md` e
  `docs/game-internals.md`.

## Arquivos relevantes

| Arquivo | Responsabilidade |
|---|---|
| `src/game/vcs_world_hd.cpp` | Experimento atual; não confiar nos slots/fila/listas existentes. |
| `src/game/vcs_world.cpp` | Hooks de observação de streaming de áreas; melhor ponto para instrumentação segura. |
| `src/game/vcs_distance.cpp` | LOD/visibilidade e prefetch de modelos, separado do streaming de células. |
| `src/game/vcs_memory.cpp` | Heap de streaming de modelos acima da RAM original; não aumenta buffer das áreas. |
| `docs/game-internals.md` | Fonte de verdade de endereços e descobertas; atualizar na mesma mudança. |
| `docs/FORK.md` T2 | Escopo da feature e caminhos aceitos. |
| `docs/T2_POPULATION_AND_DISTANT_LIFE_PLAN.md` | Restrições para população/proxies distantes; não clample peds/carros reais. |
| `tools/eedecomp.py` | Decompilação read-only do projeto Ghidra já analisado. |

## Regras que o próximo agente não deve violar

- Não chamar uma função recompiled guest diretamente a partir de uma thread
  host, nem assumir que ela retorna quando o corpo lógico termina.
- Não usar campos de estado do `level` como arrays extras de estruturas.
- Não concatenar listas de área no draw sem reproduzir integralmente as
  invariantes do merge de cross-fade original.
- Não apagar o caminho emulado/original; toda substituição deve ter switch de
  fallback e referência comparável.
- Não testar sobre o memory card real do usuário.
- Não concluir que mais heap de `CStreaming` aumenta a área detalhada: heap de
  modelos/texturas e buffers de área são subsistemas diferentes.
