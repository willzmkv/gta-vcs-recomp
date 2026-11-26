// Native C++ reimplementation of GTA VCS's CRenderer, CVisibilityPlugins,
// and world/frustum/box culling functions (Passo 2.2).
// Enabled by default; VCS_NATIVE_RENDERER=0 falls back to recompiled EE.
// VCS_NATIVE_RENDERER_VERIFY=1 runs dual execution on verified routines and checks diffs.

#pragma once

class PS2Runtime;

namespace vcs::renderer
{
    void install(PS2Runtime &runtime);
    void onFrameKick();
} // namespace vcs::renderer
