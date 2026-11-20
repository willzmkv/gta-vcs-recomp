# Menu global do jogo (T12) — plano

> **Status:** PLANEJADO (17/11/2026) · entra na 1.0.0 (decisão do usuário: pode atrasar o lançamento)
> **Repositório:** branch `hd-world-merge`

## Objetivo

ESC (teclado) ou Start (gamepad) abre **um menu de PC completo** que substitui o menu de pausa do jogo e o F2:
todas as opções do menu original, adaptadas ao port, mais as nossas, no visual do launcher. F2 e F5 deixam de
existir. ## Decisões do usuário (17/11)

1. **Motor visual do launcher**, portado para dentro do jogo (não ImGui).
2. **Tudo redesenhado**: Mapa, Missão (briefings), Estatísticas e Salvar/Carregar também no nosso estilo.
3. **Só ESC/Start**: F2 (configurações) e F5 (trainer) somem; o trainer vira uma aba do menu.
4. **Um menu real**: o jogo pausa de verdade (mundo, relógio, scripts, áudio e rádio parados, como no menu
   original) e o menu ocupa a tela inteira - não é um painel por cima do jogo rodando.

## Como (arquitetura)

- **Um kit de UI compartilhado.** `launcher/render` (lista de instâncias + `ui.hlsl` com SDF + atlas de glifos
  DirectWrite/FreeType + atlas de imagens) e `launcher/ui` (widgets imediatos, tema, animação) viram uma biblioteca
  usada pelo launcher e pelo jogo, sem mudar o comportamento do launcher (conferido por screenshots `--screenshot`
  antes e depois, pixel a pixel).
- **Um backend novo no jogo**: o mesmo `ui.hlsl` (DXBC para D3D12/D3D11, SPIR-V para Vulkan, pelos scripts do
  projeto) desenhado pelo RHI do jogo como overlay, depois do quadro e antes do present — como o ImGui do F2 hoje.
- **Pausa e entrada.** Abrir o menu pausa o jogo como o menu original faz (rádio, relógio, scripts) e toma o
  teclado, o mouse e o gamepad; fechar devolve tudo. O frontend original nunca é desenhado.
- **Lógica do jogo por baixo.** Salvar, carregar e apagar continuam sendo feitos pelas rotinas do próprio jogo
  (as ações do frontend, game-internals 6m e 10), só a apresentação é nossa: sem risco de corromper saves.
- **Dados do jogo lidos da memória**: blips e alvo do radar para o mapa, os briefings (textos GXT convertidos),
  as estatísticas (os valores e rótulos que a página original usa), os slots do cartão.

## Abas

| Aba | Conteúdo | Origem |
|---|---|---|
| Mapa | mapa da cidade com zoom/arraste por mouse e stick, blips, legenda, marcador de destino | texturas do mapa e o radar do jogo |
| Missão | os últimos briefings | o histórico de mensagens do jogo |
| Estatísticas | progresso, crimes, recordes | a página de estatísticas do jogo |
| Jogo | salvar, carregar, apagar, sair para o Windows | rotinas de cartão do jogo |
| Vídeo | presets (os do launcher), resolução, MSAA, widescreen, HUD, brilho, legendas, radar, rastros, posição da tela | nossas settings + as do jogo |
| Distância e população | mundo HD, distâncias, densidades, crowd bands | nossas settings |
| Áudio | volumes (mestre, música, efeitos, filmes), saída, em segundo plano | nossas + as do jogo |
| Controles | esquema PS2 / moderno com o desenho do controle (como no launcher), teclas, mouse, gamepad | nossas + as do jogo |
| Mods | mods do modloader, prioridades, troca ao vivo | F2 > Mods |
| Trainer | o que hoje é o F5 | `vcs_trainer` |

## Fases

| Fase | Entrega | Estimativa |
|---|---|---|
| 0 Desenho | as telas montadas no próprio launcher com dados de exemplo; screenshots para aprovação do usuário | 2 dias |
| 1 Kit compartilhado + backend no jogo | um painel de teste do kit desenhado dentro do jogo em D3D12, D3D11 e Vulkan | 3 dias |
| 2 Casca do menu | ESC/Start abre e pausa; navegação por teclado/mouse/gamepad; abas de configurações completas; F2/F5 removidos | 3 dias |
| 3 Jogo | salvar / carregar / apagar / sair pelo nosso menu | 2 dias |
| 4 Missão e Estatísticas | textos do jogo convertidos, listas roláveis | 2 dias |
| 5 Mapa | mapa, zoom, arraste, blips, legenda, destino | 4 dias |
| 6 Testes e acabamento | `menu_check.py` reescrito, controles, saves, missões; revisão visual com o usuário | 3 dias |

Total estimado: ~3 semanas. **Adiado para a 2.0.0** (decisão do usuário, 19/11/2026): na 1.0.0 o F2 ganha a cara do launcher (#80).

## Riscos

- Ler o mapa e os briefings da memória do jogo exige engenharia reversa nova (blips, GXT): mitigado começando cedo
  (fase 0 em paralelo).
- Pausar o jogo sem o frontend: se a pausa depender do frontend, o menu original é aberto invisível e só a
  apresentação é nossa.
- O launcher não pode regredir: a extração do kit é verificada por screenshots idênticos.
