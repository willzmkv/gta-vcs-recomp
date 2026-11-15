# Design Spec: VCS.rec Launcher (Estilo Rockstar Games Launcher)

**Data:** 2026-11-14  
**Projeto:** `Grand Theft Auto: Vice City Stories (PC) — VCS.rec` (`D:\Games\vcs-hd-world-impl`)  
**Alvo:** `launcher/` (`VCSLauncher.exe`)

---

## 1. Visão Geral e Objetivo

Construir um launcher nativo, ultraleve e independente (`VCSLauncher.exe`) inspirado na arquitetura visual do **Rockstar Games Launcher** e fiel ao mockup fornecido pelo autor do fork (**W.mkv / VCS.rec**), mantendo a base C++20 com renderização direta em hardware (**Direct3D 11** no Windows e **Vulkan** no Linux) em um único *draw call* instanciado.

O launcher substitui completamente a interface antiga do projeto original por uma identidade própria (**W.mkv** / **VCS.rec**), integrando:
1. **Tela Principal (`JOGO`)** com a arte de fundo de Vice City Stories, logo oficial, botão primário `▶ PLAY GAME` e os 4 botões translúcidos laterais (`SELECT GAME`, `BUILD GAME`, `CONFIGURATION`, `ADD TO STEAM`).
2. **Gerenciador e Inspetor de Mods (`MODS`)** que ativa/desativa mods em `modloader/`, ajusta prioridades em `modloader/modloader.ini` e classifica automaticamente a compatibilidade de cada mod (*Compatível*, *Parcialmente Compatível* ou *Incompatível*).
3. **Configurações Reais do Jogo (`CONFIGURAÇÕES`)** sincronizadas diretamente com `build/vcs.ini`, exibindo o **custo real de desempenho** (CPU, GPU, RAM/VRAM) de cada opção gráfica e de mundo.
4. **Integração Automática com a Steam (`ADD TO STEAM`)** que registra o `VCSLauncher.exe` no `shortcuts.vdf` da Steam já pré-configurado com **Hero**, **Grid (Capa)**, **Logo**, **Ícone** e título oficial.

---

## 2. Identidade Visual e Assets Customizados

Os assets fornecidos em `C:\Users\dejot\Desktop\Mockup Launcher` serão incorporados ao pipeline do launcher (`launcher/assets/`):

| Arquivo Fonte | Destino no Projeto | Uso na Interface e Sistema |
| :--- | :--- | :--- |
| `minha_logo.png` | `launcher/assets/minha_logo.png` | Logo **W.mkv** no topo da barra lateral esquerda (substitui o logo da Rockstar). |
| `icone.png` | `launcher/assets/icone.png` & `launcher.ico` | Card do jogo na barra lateral ("Meus jogos"), ícone `.ico` do executável/janela e ícone da Steam (`<appid>_icon.png`). |
| `fundo.png` | `launcher/assets/fundo.png` | Wallpaper principal de fundo na área direita do launcher. |
| `logo.png` | `launcher/assets/logo.png` | Logo transparente *Grand Theft Auto: Vice City Stories* acima do botão `PLAY GAME` e logo da Steam (`<appid>_logo.png`). |
| `grid.png` | `launcher/assets/grid.png` | Capa vertical (`600x900`) copiada automaticamente para a Steam (`<appid>p.png`). |
| `hero.png` | `launcher/assets/hero.png` | Banner largo (`1920x620`) copiado automaticamente para a Steam (`<appid>_hero.png`). |

### Paleta e Tipografia
- **Sidebar Esquerda:** Preto profundo (`#08080C`), divisória sutil (`#1E1E28`), item ativo com fundo `#2A1224` e barra vertical esquerda rosa neon (`#FF2A85`).
- **Botão Principal (`▶ PLAY GAME`):** Gradiente rosa/magenta (`#FF55A8` → `#E01E79`), borda clara `rgba(255, 255, 255, 0.55)`, brilho externo (*glow*) rosa e texto branco em caixa alta.
- **Botões Laterais (`SELECT GAME`, `BUILD GAME`, `CONFIGURATION`, `ADD TO STEAM`):** *Glassmorphism* escuro translúcido (`rgba(20, 18, 30, 0.78)`), borda `1.5px` (`rgba(255, 255, 255, 0.42)`), ícone vetorial à esquerda e rótulo alinhado em destaque.
- **Rodapé (`ⓘ`):**
  - Título: **Grand Theft Auto: Vice City Stories (PC)**
  - Subtítulo: `Versão recompilada (VCS.rec) • Build 1.0.0` (acompanhado do estado atual do disco/executável).

---

## 3. Arquitetura de Renderização de Imagens (D3D11 & Vulkan)

Para desenhar `fundo.png`, `logo.png`, `minha_logo.png` e `icone.png` com alta qualidade e cantos arredondados sem adicionar dependências externas pesadas:

1. **Atlas de Imagens RGBA8 (`image_atlas`):**
   - No carregamento do launcher, os 4 PNGs de interface (`fundo.png`, `logo.png`, `minha_logo.png`, `icone.png`) são decodificados para RGBA8 (usando WIC `IWICImagingFactory` no Windows / `png::decode` expandido para RGBA8) e empacotados em uma textura 2D RGBA8 (`Texture2D<float4> image_atlas : register(t1)` no D3D11 e binding correspondente no Vulkan).
2. **Nova Primitiva `KIND_IMAGE = 6` (`launcher/render/draw.h`, `ui.hlsl`):**
   - Usa o SDF de caixa arredondada (`sd_rounded_box`) já existente no pixel shader para recortar cantos arredondados com anti-aliasing perfeito (ex.: `radius` no card do ícone e na logo `W.mkv`).
   - Amostra `image_atlas` nas coordenadas UV interpoladas e multiplica pela cor/opacidade do vértice (permitindo *fade-in*, *hover* e escurecimento suave do fundo nas abas `MODS`, `CONFIGURAÇÕES` e `SOBRE`).

---

## 4. Navegação e Telas do Launcher

### 4.1. Barra Lateral Esquerda (Fixa)
- **Topo:** Logo `minha_logo.png` (**W.mkv**) + título **"W.mkv Launcher"** / **"VCS.rec"**.
- **Seção `Meus jogos`:** Card selecionável com `icone.png`, barra rosa neon ativa e texto `Grand Theft Auto: Vice City Stories`.
- **Rodapé da Sidebar:** Indicador compacto de versão (`VCS.rec`) e atalho rápido para abrir a pasta do jogo.

### 4.2. Barra Superior (Topbar)
- **Abas de Navegação:** `JOGO` | `MODS` | `CONFIGURAÇÕES` | `SOBRE` (aba ativa em destaque com indicador rosa neon `#FF2A85` abaixo do texto).
- **Área de Arraste + Controles de Janela:** Minimizar (`—`), Maximizar/Restaurar (`□`) e Fechar (`✕`).

### 4.3. Aba `JOGO` (Home)
- Exibe `fundo.png` em tela cheia na área direita, `logo.png` posicionado à esquerda sobre o carro e o botão **`▶ PLAY GAME`** logo abaixo.
- À direita, os 4 botões no estilo Rockstar:
  1. **`SELECT GAME`**: Abre diálogo nativo para selecionar a ISO de PS2 (`SLUS-21590`) ou pasta de build. Valida o SHA-1 do executável `SLUS_215.90` (`29732459D82330633202863FB7AC286E22F1DBBC`) e grava `iso=<caminho>` em `build/vcs.ini`.
  2. **`BUILD GAME`**: Inicia `tools/build.py` (ou abre o painel de progresso de compilação quando em execução), exibindo barra de progresso das 6 etapas (verificar ferramentas, extrair ELF, gerar código C++, configurar CMake, compilar `vcs.exe`, copiar assets) e botão para ver log/cancelar.
  3. **`CONFIGURATION`**: Alterna imediatamente para a aba **`CONFIGURAÇÕES`**.
  4. **`ADD TO STEAM`**: Executa a integração automática com a Steam e exibe confirmação visual no próprio botão/rodapé.

### 4.4. Aba `MODS` (Gerenciador + Inspetor de Compatibilidade)
Escaneia `<pasta_do_jogo>/modloader/` e gerencia tanto `build/vcs.ini` quanto `build/modloader/modloader.ini`:
- **Controles Superiores:**
  - Toggle Mestre: **Mod Loader (`mods.enabled`)**
  - Toggle: **Substituição de Texturas HD (`graphics.texture_replacements`)**
  - Botões: **`Abrir pasta modloader/`** e **`Recarregar lista`**
- **Lista de Mods com Inspetor de Compatibilidade:**
  Para cada subpasta em `modloader/`, o launcher inspeciona recursivamente as extensões e estruturas de arquivos:
  - 🟢 **`COMPATÍVEL`**:
    - Contém texturas `.dds` ou `.png` (pacotes de texturas PCSX2 / `replacements`), arquivos `.mdl`/`.xtx`/`#<id>.*` (`GTA3PS2.IMG`), arquivos soltos do disco (`.gxt`, `.scm`, `.dat`, `.cfg`, `.txd` nativo) ou pares `.dff` + `.txd` de veículos/armas de GTA Vice City (PC) suportados pelo conversor `rwconv`.
  - 🟡 **`PARCIAL`**:
    - Contém arquivos compatíveis misturados com arquivos ignorados pelo motor (ex.: `.txd` de PC sem o `.dff` correspondente do veículo/arma, ou `.col`/`.ipl`/`.ide` de mapa de PC). Exibe aviso detalhando o que será carregado e o que será ignorado.
  - 🔴 **`INCOMPATÍVEL`**:
    - Contém binários/scripts de injeção de PC x86 (`.asi`, `.dll`, `.cleo`, `.cs`, `.cm`, `.exe`) ou apenas arquivos não suportados. Informa claramente que `VCS.rec` é uma recompilação nativa de PS2 e não carrega plugins `.asi`/CLEO de GTA VC PC.
- **Controles por Mod:**
  - Checkbox Ativar/Desativar (`mods.<nome_normalizado>=0|1` em `vcs.ini`).
  - Controle de Prioridade (`1` a `100`, padrão `50`, salvo em `modloader/modloader.ini` sob `[Profiles.Default.Priority]`).

### 4.5. Aba `CONFIGURAÇÕES` (Sincronização com `build/vcs.ini` + Custo Real de Desempenho)
Lê todas as chaves existentes em `build/vcs.ini` (preservando chaves de controles, janelas e estados personalizados ao salvar) e apresenta cada configuração dividida em seções claras, com **Presets Rápidos** (*Autêntico PS2*, *Aprimorado PC*, *Leve / Integrada*) e **Badge de Custo de Desempenho**:

1. **Tela e Taxa de Quadros (`Display`)**:
   - `display.window`: Janela (`0`) / Tela Cheia (`1`) — 🟢 *Custo Nulo*
   - `display.aspect`: 4:3 (`43`), 16:10 (`1610`), 16:9 (`169`), 21:9 (`219`), 32:9 (`329`), Ajustar à Janela (`0`) — 🟢 *Custo Nulo*
   - `display.hud`: Manter 4:3 (`0`), Bordas da Tela (`2`), Esticar (`1`) — 🟢 *Custo Nulo*
   - `display.cutscene_bars`: Barras pretas nas cutscenes (`0`/`1`) — 🟢 *Custo Nulo*
   - `display.timing`: Original PS2 (`0`) / Independente de FPS (`1`) — 🟢 *Custo Nulo*
   - `display.fps`: 30, 60, 100, 120, 144, 165, 240, Taxa do Monitor (`-1`), Ilimitado (`0`) — 🟡 *Custo Médio (CPU/GPU acima de 60 FPS)*
   - `display.vsync`: Desativado (`0`), Ativado (`1`), VRR Sem Tearing (`2`) — 🟢 *Custo Nulo*
   - `display.fps_counter`: Mostrar FPS (Desativado / Cantos da tela `0..4`) — 🟢 *Custo Nulo*
2. **Qualidade Gráfica e Renderizador (`Graphics`)**:
   - `display.renderer`: D3D12 (`1`), D3D11 (`0`), Vulkan (`2`) — 🟢 *Ganho de CPU no D3D12 em ruas densas*
   - `display.scale`: 1x (`1`), 2x (`2`), 3x (`3`), 4x (`4`), 6x (`6`), 8x (`8`), Ajustar à Janela (`0`) — 🔴 *Custo Alto (GPU Fillrate)*
   - `display.msaa`: Desativado (`1`), 2x (`2`), 4x (`4`), 8x (`8`) — 🟠 *Custo Médio-Alto (GPU/VRAM)*
   - `display.aniso`: Bilinear (`0`), 2x (`2`), 4x (`4`), 8x (`8`), 16x (`16`) — 🟢 *Custo Baixo (GPU)*
   - `graphics.post_process` & `graphics.exposure`: Pós-processamento (`0`/`1`) e Exposição (`0..200%`) — 🟢 *Custo Baixo (GPU)*
3. **Distância, Neblina e Mundo HD (`World & Distance`)**:
   - `world.hd_radius`: Cidade Detalhada HD — Original ~260m (`0`), ~450m (`1`), ~580m (`2`), ~720m (`3`) — 🔴 *Custo Alto (CPU + RAM + Draw Calls)*
   - `world.view_distance`: Distância de Visão de Objetos (`0..100%`) — 🟠 *Custo Médio-Alto (CPU/GPU)*
   - `world.detail_distance`: Distância de Detalhe LOD (`0..100%`) — 🟠 *Custo Médio (GPU)*
   - `fog.distance` (`10..100%`) & `fog.start` (`0..300%`): Neblina de distância — 🟢 *Custo Nulo*
4. **População, Tráfego e Memória (`Population & Memory`)**:
   - `memory.streaming`: Memória de Streaming — PS2 5.4 MB (`0`), 12 MB (`12`), 20 MB (`20`), 30 MB Recomendado (`30`) — 🟢 *Custo Baixo (Apenas RAM; essencial para densidades altas e mods)*
   - `world.population_distance`: Distância da População (`0..100%`) — 🔴 *Custo Alto (CPU/EE)*
   - `world.vehicle_density`: Densidade de Veículos (`0..100%`) — 🔴 *Custo Alto (CPU/Memória)*
   - `world.pedestrian_density`: Densidade de Pedestres (`0..100%`) — 🔴 *Custo Alto (CPU Skinning/IA)*
   - `world.crowd_bands`: Bandas de Multidão (`0`/`1`) — 🟢 *Otimizador (Reduz custo de CPU de pedestres distantes)*
   - `world.fps_target`: Alvo de FPS da População (`0`=Off, `45`, `60`, `70`, `90`, `120`, `144`) — 🟢 *Otimizador Dinâmico*
   - `world.distant_traffic` (`0..100`) & `world.distant_crowds` (`0..100`): Tráfego e multidões distantes decorativas — 🟡 *Custo Baixo-Médio (Sem física/IA)*
5. **Áudio e Melhorias (`Audio & Enhancements`)**:
   - `audio.volume` (`0..100%`), `audio.movie_volume` (`0..100%`), `audio.output` (Estéreo/Mono/Invertido)
   - `display.skip_intro` (`0`/`1`), `game.background` (Continuar/Mudo/Pausar)
   - `enhance.button_prompts` (Original/Auto/PlayStation/Xbox/Teclado), `enhance.driveby_rate` (PS2/Taxa da arma)

### 4.6. Aba `SOBRE`
- Exibe informações do projeto **Grand Theft Auto: Vice City Stories (PC) — VCS.rec**, créditos do fork (**W.mkv**), resumo da arquitetura de recompilação estática PS2 → PC nativo, caminhos ativos (`build/vcs.exe`, `build/vcs.ini`, `modloader/`) e botões rápidos para abrir a pasta de saves (`%LOCALAPPDATA%\VCSRecomp`) e pasta do jogo.

---

## 5. Especificação Técnica da Integração `ADD TO STEAM`

1. **Descoberta da Instalação da Steam:**
   - Windows: Consulta `HKCU\Software\Valve\Steam\SteamPath` (ou `HKLM\SOFTWARE\WOW6432Node\Valve\Steam\InstallPath`), além de `C:\Program Files (x86)\Steam`.
   - Linux: `~/.steam/steam` ou `~/.local/share/Steam`.
2. **Varredura de Contas (`<SteamPath>/userdata/<AccountId>/config/`):**
   - Para cada pasta numérica `<AccountId>` em `userdata/` (ignorando `0` / `ac`):
     - Garante a existência de `config/` e `config/grid/`.
3. **Cálculo do AppID de Atalho Não-Steam:**
   - Segue o algoritmo padrão do cliente Steam:
     - String base: `"\"" + caminhoAbsolutoExe + "\"" + "Grand Theft Auto: Vice City Stories"`
     - `uint32_t appId = crc32_ieee(base) | 0x80000000u;`
4. **Atualização Segura do Binary VDF (`shortcuts.vdf`):**
   - Faz o parse da árvore binária do `shortcuts.vdf` existente (tipos `0x00` sub-mapa, `0x01` string, `0x02` uint32, `0x08` fim de mapa).
   - Se já houver um atalho apontando para `VCSLauncher.exe` ou `Grand Theft Auto: Vice City Stories`, atualiza os campos (`appid`, `AppName`, `Exe`, `StartDir`, `icon`); caso contrário, adiciona um novo índice numérico ao final preservando todos os outros atalhos do usuário.
5. **Cópia Automática das Artes para `config/grid/`:**
   - `<appId>p.png` ← `grid.png` (Capa vertical 600x900)
   - `<appId>_hero.png` ← `hero.png` (Banner Hero 1920x620)
   - `<appId>_logo.png` ← `logo.png` (Logo oficial transparente)
   - `<appId>_icon.png` ← `icone.png` (Ícone quadrado com borda arredondada)

---

## 6. Testes e Verificação

- **Compilação Nativa:** `python tools/build.py --launcher` deve compilar sem erros ou warnings, embutindo os shaders atualizados (`ui.hlsl` → `shader_dxbc.h` / `shader_spirv.h`) e os assets de imagem.
- **Self-Test Automatizado (`--self-test`):** Valida inicialização da janela, parser de `vcs.ini`, scanner de compatibilidade de mods e gerador de `shortcuts.vdf` / cálculo de CRC32 AppID.
- **Inspeção Visual (`--screenshot`):** Captura screenshots de todas as abas (`JOGO`, `MODS`, `CONFIGURAÇÕES`, `SOBRE`) para validação visual comparada ao mockup do usuário.
