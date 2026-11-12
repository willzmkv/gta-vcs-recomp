// [gta-recomp] Hardware GS renderer (D3D11 through the RHI, src/rhi; was OpenGL 3.3).
//
// The game thread records: GSHwBackend implements GSRasterBackend, keeps VRAM, transfers and CLUTs in an
// embedded GSCpuBackend (everything but drawing), turns primitives into batched draws, decodes textures
// (cached) and hands each frame's command list over at VBlank. The main thread executes the lists on the
// window's RHI device (render targets, depth, blending, alpha test, fog) and shows the display circuit.
//
// PS2X_GS=hw selects it. PS2X_HW_SCALE=<1..8> renders at that multiple of the PS2 resolution.
#pragma once

#include <string>

#include "runtime/gs/gs_backend.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace rhi
{
    class Device;
    class Texture;
}

struct GSHwOptions
{
    uint32_t scale = 1;
    bool nativeFrame = false; // [gta-recomp] PS2X_HW_SCALE=0: the picture's targets at the window's resolution
    uint32_t msaa = 1;        // [gta-recomp] PS2X_HW_MSAA: samples for the picture's targets (1 = off)
    uint32_t anisotropy = 0;  // [gta-recomp] PS2X_HW_ANISO: anisotropic filtering of the game's textures (0 = off)
    static GSHwOptions fromEnvironment();
};

// Main thread side.
class GSHwPresenter
{
public:
    virtual ~GSHwPresenter() = default;
    // Main thread, with the window's device: run every frame recorded since the last call. Returns the texture
    // holding the newest displayed picture (alpha = 1, row 0 at the top; owned by the renderer, valid until the next
    // call) and its size; false until a first frame has been displayed.
    virtual bool executeAndGetDisplay(rhi::Device &device, rhi::Texture *&texture, uint32_t &width, uint32_t &height) = 0;
    // [gta-recomp] Whether the displayed frames draw 3D (depth-tested or native draws), with some hysteresis: frames that
    // are only 2D (loading screens, menus) are presented at 4:3 in widescreen.
    virtual bool showing3d() const = 0;
    // [gta-recomp] One line about the frames composed last (draws, native draws, 3D, displayed buffer): diagnostics.
    virtual std::string lastFrameInfo() const = 0;
};

class GSHwBackendHandle : public GSRasterBackend
{
public:
    // Game thread, at VBlankStart: close the frame's command list (privRegs: PMODE..BGCOLOR in GS order).
    virtual void endFrame(const uint64_t (&privRegs)[15]) = 0;
    virtual GSHwPresenter &presenter() = 0;
};

std::unique_ptr<GSHwBackendHandle> makeHwRasterBackend(const GSHwOptions &options);

// [gta-recomp] Widescreen (docs/SETTINGS_MENU_WIDESCREEN.md): 2D draws (no depth test, into a 640-wide target, not
// covering the whole width) are squeezed horizontally by `factor` around the middle, so the HUD keeps its 4:3 shape in a
// wider picture. 1 = off. Any thread.
void gsHwSetHudSqueeze(float factor);

// [gta-recomp] Per-element HUD placement in widescreen: the game names a 2D primitive's anchor from its vertices'
// positions (x, y pairs in pixels, XYOFFSET removed). Squeezed (as above) towards the anchor: the left edge, the right
// edge or the middle; Stretch keeps the 640-wide position (full-screen draws). None = the squeeze heuristic alone.
// Called on the thread that processes GS packets; set it from any thread. nullptr = off.
enum : uint8_t
{
    GSHwAnchorNone = 0,
    GSHwAnchorCentre,
    GSHwAnchorLeft,
    GSHwAnchorRight,
    GSHwAnchorStretch,
    // [gta-recomp] or'ed in: the primitive is the game's menu. A frame with menu primitives and no 3D is shown at 4:3 at
    // once (the pause menu), not after the half-second the 3D history takes.
    GSHwAnchorMenu = 0x80,
};
using GSHwAnchorFn = uint8_t (*)(const float *xy, uint32_t count);
void gsHwSetHudClassifier(GSHwAnchorFn fn);

// [gta-recomp] Button prompt glyphs (src/game/vcs_prompts.cpp): a 2D primitive the game drew as a button glyph is drawn
// from a host-made glyph atlas instead of the game's texture. The classifier gets the primitive's vertices (x, y pairs in
// pixels, XYOFFSET removed, as for the HUD classifier) and, for a glyph, fills `hit`: per vertex the atlas coordinates
// (0..1) and an x offset in pixels (a glyph wider than the game's box grows to the left), and whether it is the text's
// drop shadow (drawn as a dark silhouette). Called on the thread that processes GS packets; nullptr = off.
struct GSHwGlyphHit
{
    float u[8], v[8], dx[8];
    bool shadow;
};
using GSHwGlyphFn = bool (*)(const float *xy, uint32_t count, GSHwGlyphHit &hit);
void gsHwSetGlyphClassifier(GSHwGlyphFn fn);
// [gta-recomp] The glyph atlas (RGBA8, alpha in the PS2's 0..128 scale, w and h powers of two), taken by the next glyph
// draw. Call it on the thread that processes GS packets (through ps2_render_worker::runOnGsStage), so the frames
// recorded after it - whose classifier answers name its cells - draw from it.
void gsHwSetGlyphAtlas(uint32_t width, uint32_t height, std::shared_ptr<const std::vector<uint32_t>> pixels);
// [gta-recomp] Glyph draws recorded so far (the prompts check).
uint64_t gsHwGlyphDraws();

// [gta-recomp] Native HUD layer: the primitives the classifier names are kept out of the PS2 frame buffer and drawn over
// the displayed picture at the output size (gsHwSetOutputSize) - sharp at any window size, in its aspect. The displayed
// buffer's latest HUD is drawn; a new scene in that buffer starts the next one. Any thread.
void gsHwSetHudLayer(bool on);
// [gta-recomp] Widescreen: how much wider than 4:3 the game's 640-wide frame is shown (display aspect / (4/3)); the
// native post effects widen their buffers by it so blur and warp keep their 4:3 proportions on screen. Any thread.
void gsHwSetAspectWidening(float widening);
// [gta-recomp] Frame pacing between the game and the window. Off (the PS2's timing): the game may run 3 frames ahead and
// each host frame renders every queued frame, showing the last - game time keeps its pace when the GPU falls behind.
// On (frame-rate independent timing, where game time follows the clock anyway): one frame queued, one rendered and
// shown per host frame - a GPU-bound machine shows every frame it renders (the HD 520: 76 shown fps instead of 30 at the
// same GPU load; docs/PERFORMANCE.md 17) with the least latency. Any thread.
void gsHwSetShowEveryFrame(bool on);
namespace render
{
    class TextureReplacements;
}
// [gta-recomp] Texture packs (docs/TEXTURE_PACKS.md, the mod loader src/mods): the replacements to use from the GS
// stage's next frame on (null: none - the game's own textures). The old pack's textures are dropped then, and the
// game's or the new pack's come back as they are drawn. Any thread; the caller indexed the pack.
void gsHwSetTextureReplacements(std::shared_ptr<render::TextureReplacements> pack);
// [gta-recomp] The input snapshot time (PSPadBackend::lastInputNs) of the frame the last host frame showed: for the
// input-to-screen delay (rhi::Device::setFrameInputTime). Main thread.
int64_t gsHwShownInputNs();
// [gta-recomp] How long a frame takes from its input snapshot to ready for the window (game, render worker, GS stage):
// the 95th percentile of recent frames, 0 before any (the frame lead, docs/PERFORMANCE.md 19). Any thread.
int64_t gsHwFrameBuildNs();
// [gta-recomp] Native frame (GSHwOptions::nativeFrame): the size the picture's own targets are drawn at - the window's
// letterboxed area for 3D frames' aspect. 0 = the integer scale. Any thread.
void gsHwSetFrameSize(uint32_t width, uint32_t height);
// [gta-recomp] The size the picture is shown at (the window's letterboxed area); 0 = the render targets' size.
void gsHwSetOutputSize(uint32_t width, uint32_t height);
// [gta-recomp] Render targets named by what they are (docs/FULLY_NATIVE.md step 3d): the game says which buffer (FBP,
// FBW) is which. windowScale: drawn at the native frame's scale (gsHwSetFrameSize) like the picture's own targets
// instead of 1x - for a buffer only sampled, never shown or copied 1:1, whose detail grows with it (VCS's reflection
// map); kept at 1x while the GS bloom (PS2X_HW_POSTFX without b) shares its memory. The names show in PS2X_HW_RT_LOG.
// Main thread, before rendering; up to 16.
void gsHwSetTargetRole(uint32_t fbp, uint32_t fbw, const char *name, bool windowScale);
// [gta-recomp] A target whose minifying copies from another render target are read filtered: a tent over each target
// pixel's whole footprint in the source, at the source's resolution, instead of the GS's four bilinear taps (VCS's
// reflection map: "Car reflections: Smooth", docs/game-internals.md 6g). mode 0 off (the GS's read), 1 always, 2 when
// the source is drawn above 1x (its vertical scale: widescreen's 1x is wider, not finer). Any thread, any time; up to 4.
void gsHwSetTargetFilter(uint32_t fbp, uint32_t fbw, int mode);
// [gta-recomp] Texture identity by raster (docs/FULLY_NATIVE.md step 3d): the RAM address of the raster the next upload on
// this thread sends, and its byte count - taken by the next upload of exactly that many bytes; a texture made from that
// upload is named by the address (gsHwTextureForRaster). The thread that processes the VIF1 chain.
void gsHwSetUploadSource(uint32_t address, uint32_t bytes);
// [gta-recomp] this thread's upload source, cleared: handed to the GS stage with the next command (gs_frontend.cpp)
void gsHwTakeUploadSource(uint32_t &address, uint32_t &bytes);
// [gta-recomp] The renderer's texture key of the raster at `address` (the latest made from it), 0 if none. Render thread.
uint64_t gsHwTextureForRaster(uint32_t address);
// [gta-recomp] Draw sources (milestone 3's "unconverted draws"): the draws still made from GS primitives, by kind -
// where their packets came from (runtime/gs/gs_draw_source.h), target, texture, blend, depth, coverage - per frame
// since the last call, one kind per line, most draws first; `reset` starts a new window. The first call switches the
// per-kind table on (PS2X_HW_DRAW_SOURCES=1 does from boot and logs it every 5 s). The totals are always in the perf
// line (gs_draws ...). Main thread.
std::string gsHwDrawSourceReport(bool reset);
