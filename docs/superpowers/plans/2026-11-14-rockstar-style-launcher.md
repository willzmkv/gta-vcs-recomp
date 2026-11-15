# VCS.rec Rockstar-Style Launcher Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Transform `VCSLauncher.exe` into the custom **W.mkv / VCS.rec** Rockstar-style launcher matching the user's mockup, with RGBA artwork rendering, real `build/vcs.ini` settings + performance cost badges, a `modloader/` manager with automatic compatibility inspection, and one-click Steam shortcut + artwork registration.

**Architecture:** Extends the existing self-contained C++20 launcher (`launcher/`) with an RGBA8 texture atlas (`KIND_IMAGE = 6` in `ui.hlsl`, D3D11, and Vulkan), a new `launcher/model/game_config` module handling `vcs.ini`, `modloader/`, and Steam binary `shortcuts.vdf` + `config/grid/` artwork, and a redesigned Rockstar-style sidebar + topbar + 4-tab layout (`JOGO`, `MODS`, `CONFIGURAÇÕES`, `SOBRE`).

**Tech Stack:** C++20, Direct3D 11 (Windows), Vulkan (Linux), HLSL (compiled via `tools/hlsl_dxbc.py` & `tools/hlsl_spirv.py`), `stb_image.h` (PNG decoding), CMake, Python 3.

---

### Task 1: Copy Custom Assets, Generate `launcher.ico`, and Add Vector Icons

**Files:**
- Create: `launcher/assets/fundo.png`
- Create: `launcher/assets/logo.png`
- Create: `launcher/assets/minha_logo.png`
- Create: `launcher/assets/icone.png`
- Create: `launcher/assets/grid.png`
- Create: `launcher/assets/hero.png`
- Modify: `launcher/launcher.ico`
- Modify: `launcher/launcher.png`
- Modify: `launcher/ui/theme.h`
- Modify: `launcher/render/icons.cpp`

- [ ] **Step 1: Copy user's 6 PNG assets into `launcher/assets/` and generate `launcher.ico` from `icone.png`**
- [ ] **Step 2: Add `kPlayFilled`, `kCrossedTools`, `kSteam`, and `kMod` icon codepoints in `launcher/ui/theme.h` and `launcher/render/icons.cpp`**

---

### Task 2: RGBA8 Image Atlas & `KIND_IMAGE` Shader/Renderer Support

**Files:**
- Modify: `launcher/core/png.h`
- Modify: `launcher/core/png.cpp`
- Modify: `launcher/render/draw.h`
- Modify: `launcher/render/draw.cpp`
- Modify: `launcher/render/d3d11/ui.hlsl`
- Modify: `launcher/render/d3d11/renderer_d3d11.cpp`
- Modify: `launcher/render/vulkan/renderer_vulkan.cpp`

- [ ] **Step 1: Add `core::readPng` and `core::resizeRgba` in `launcher/core/png.h` and `launcher/core/png.cpp` using `third_party/stb/stb_image.h`**
- [ ] **Step 2: Add `kImage = 6`, `ImageId`, `ImageAtlas`, `imagesInit`, `image`, and `imageCover` to `launcher/render/draw.h` and `launcher/render/draw.cpp`**
- [ ] **Step 3: Add `KIND_IMAGE` and `Texture2D<float4> image_atlas : register(t1)` to `launcher/render/d3d11/ui.hlsl`**
- [ ] **Step 4: Bind and upload `image_atlas` at slot `t1` in `launcher/render/d3d11/renderer_d3d11.cpp` and binding `17` in `launcher/render/vulkan/renderer_vulkan.cpp`**

---

### Task 3: Game Settings (`vcs.ini`), Mod Compatibility Scanner, and Steam Integration (`shortcuts.vdf`)

**Files:**
- Create: `launcher/model/game_config.h`
- Create: `launcher/model/game_config.cpp`
- Modify: `launcher/model/tasks.h`
- Modify: `launcher/model/tasks.cpp`
- Modify: `launcher/CMakeLists.txt`

- [ ] **Step 1: Implement `vcs.ini` reader/writer and structured setting specs with real performance cost badges in `launcher/model/game_config.h` and `launcher/model/game_config.cpp`**
- [ ] **Step 2: Implement `modloader/` scanner, priority editor (`modloader.ini`), and recursive file compatibility inspector (`Compatible`, `Partial`, `Incompatible`)**
- [ ] **Step 3: Implement Steam installation discovery, IEEE CRC32 AppID calculation, binary `shortcuts.vdf` parser/writer, and `config/grid/` artwork installer**
- [ ] **Step 4: Wire `game_config` into `launcher::App` and `launcher/CMakeLists.txt`**

---

### Task 4: Rockstar-Style UI Widgets, Left Sidebar, Top Navigation Bar, and `JOGO` Hero Screen

**Files:**
- Modify: `launcher/ui/theme.h`
- Modify: `launcher/ui/widgets.h`
- Modify: `launcher/ui/widgets.cpp`
- Modify: `launcher/screens/shell.h`
- Modify: `launcher/screens/screens.cpp`
- Modify: `launcher/screens/titlebar.cpp`
- Modify: `launcher/screens/home.cpp`

- [ ] **Step 1: Add Rockstar-style glass button (`ui::rockstarButton`), primary pink `PLAY GAME` button (`ui::playButton`), and integer slider (`ui::slider`) in `launcher/ui/widgets.h` and `launcher/ui/widgets.cpp`**
- [ ] **Step 2: Implement the fixed Left Sidebar (`minha_logo.png`, `"Meus jogos"`, active game card with `icone.png` and neon pink bar) and Top Bar (`JOGO`, `MODS`, `CONFIGURAÇÕES`, `SOBRE` + window controls)**
- [ ] **Step 3: Implement the `JOGO` hero view (`fundo.png`, `logo.png`, `▶ PLAY GAME`, `SELECT GAME`, `BUILD GAME`, `CONFIGURATION`, `ADD TO STEAM`, and bottom `VCS.rec` info bar)**

---

### Task 5: `MODS`, `CONFIGURAÇÕES`, and `SOBRE` Tabs

**Files:**
- Modify: `launcher/screens/sheets.cpp`
- Modify: `launcher/screens/screens.cpp`

- [ ] **Step 1: Implement `MODS` tab with master toggles, mod list, priority controls, and compatibility badges (`COMPATÍVEL`, `PARCIAL`, `INCOMPATÍVEL`)**
- [ ] **Step 2: Implement `CONFIGURAÇÕES` tab with presets (*Autêntico PS2*, *Aprimorado PC*, *Leve*), categories, interactive controls, and real performance cost badges**
- [ ] **Step 3: Implement `SOBRE` tab with `VCS.rec` info, paths, saves backup/restore access, and diagnostic tools**

---

### Task 6: Build, Self-Test, and Visual Verification

**Files:**
- Modify: `launcher/main.cpp`

- [ ] **Step 1: Add self-test assertions for `game_config` (`vcs.ini`, mod scanner, Steam CRC32/VDF) in `launcher/main.cpp`**
- [ ] **Step 2: Build `VCSLauncher.exe` with `python tools/build.py --launcher` and run `VCSLauncher.exe --self-test`**
- [ ] **Step 3: Render offscreen screenshots of `JOGO`, `MODS`, `CONFIGURAÇÕES`, and `SOBRE` via `VCSLauncher.exe --screenshot` and visually verify against the mockup**
