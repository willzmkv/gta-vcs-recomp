// Native C++ replacement of VCS's hot RenderWare / RSL submission pipeline (Passo 2.1, docs/NATIVE_CXX_REWRITE.md):
//   0x003C3E58 CEntity::Render -> 0x002C0270 RpClumpRender -> 0x0039EC18 / 0x0039E828 atomic/skin/strip submission,
//   frustum tests (0x002F7430 / 0x002F7528 / 0x002F7620), lighting (0x001A73E0..), textures/CLUT/UV (0x00408F30 /
//   0x00317FB8 / 0x002F62B8 / 0x002F6A78 / 0x00317F10 / 0x00317D48 / 0x002F7090), GS queue & draw kickers
//   (0x002F7908 / 0x002F7B30 / 0x002F7D58 / 0x002F80A0 / 0x002F7EB0), frame/txd helpers, and leaf getters.
//
// Install order (src/game/vcs_overrides.cpp): MUST be installed BEFORE vcs::distant, vcs::rslnative, vcs::rslworld, and
// debug box-clip hooks so those outer layers wrap on top and remain active when enabled.
//
// VCS_NATIVE_RSL_RENDER=0: disable Passo 2.1 hooks cleanly (runs original recompiled EE code).
// VCS_NATIVE_RSL_RENDER_VERIFY=1: snapshot state and compare native output byte-for-byte against original EE execution.
// VCS_NATIVE_RSL_UPLOAD=1: also replace 0x003298D8 natively (off by default per docs/PERFORMANCE.md section 28).
#pragma once

class PS2Runtime;

namespace vcs::rslrender
{
    void install(PS2Runtime &runtime); // main thread, before vcs::distant / vcs::rslnative / vcs::rslworld
}
