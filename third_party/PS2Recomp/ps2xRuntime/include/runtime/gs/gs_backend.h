#pragma once

#include "runtime/gs/gs_types.h"

#include <cstdint>
#include <vector>

// [gta-recomp] A draw whose untransformed vertices a game's packet-level HLE handler supplies; the renderer runs
// the matching vertex program on the GPU (docs/NATIVE_RENDERER.md, stage 4+). The GS state is the frontend's current
// one, as for a primitive the VU would have kicked.
struct GSNativeDraw
{
    enum Kind : uint32_t
    {
        VcsWorld = 1, // render/shaders/gs_draw.hlsl vs_world: prelit world (float3 pos, float3 uv, uint rgba; 32 bytes)
        VcsLit = 2,   // vs_lit: skinned + lit models (float3 skinned pos, float3 uv, float3 normal, pad; 48 bytes)
    };
    uint32_t kind = 0;
    bool transient = false;         // vertices change from draw to draw: sent with the frame, not cached
    uint64_t meshKey = 0;           // content hash of the vertex data
    const void *vertices = nullptr; // vertexCount * vertexStride bytes (read only when the mesh is not cached)
    uint32_t vertexStride = 0;
    uint32_t vertexCount = 0;
    uint32_t drawCount = 0;         // vertices drawn (a strip)
    float constants[124] = {};      // the vertex program's parameters (layout per kind, gs_draw.hlsl cbuffer Native)
    uint32_t constantCount = 0;
    // VcsLit drawn once per instance (vs_lit_inst; the distant proxies): per instance kInstanceFloats - the window
    // matrix, the clip-test matrix and the light direction rows (constants 0-15, 32-47, 48-59) - the rest shared
    static constexpr uint32_t kInstanceFloats = 44, kMaxInstances = 64; // (gs_draw.hlsl VCS_LIT_INSTANCES)
    const float *instances = nullptr;
    uint32_t instanceCount = 0;     // 0: a plain draw
};

class GSRasterBackend
{
public:
    virtual ~GSRasterBackend() = default;

    // [gta-recomp] Whether SubmitNative would take the draw - all it decides it on (the GS stage answers for it ahead,
    // runtime/ps2_render_worker.h)
    virtual bool AcceptsNative(const GSNativeDraw &draw) const
    {
        (void)draw;
        return false;
    }

    // [gta-recomp] false: not supported (the caller lets the VU run instead)
    virtual bool SubmitNative(const GSDrawState &state, const GSNativeDraw &draw)
    {
        (void)state;
        (void)draw;
        return false;
    }

    // [gta-recomp] count vertices of one triangle / strip / fan with the state they are drawn with (GS::submitPackedKick):
    // the primitives Submit would get one vertex kick at a time; kick[i] = 0 for a vertex with ADC (no primitive ends
    // there). false: not handled (the caller takes the per-vertex path).
    virtual bool SubmitPacked(const GSDrawState &state, const GSVertex *vertices, const uint8_t *kick, uint32_t count)
    {
        (void)state;
        (void)vertices;
        (void)kick;
        (void)count;
        return false;
    }

    virtual void Initialize(uint8_t *vram, uint32_t vramSize) = 0;
    virtual void Reset() = 0;

    virtual void Submit(const GSPrimitiveBatch &batch) = 0;
    virtual void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) = 0;

    virtual void BeginTransfer(const GSTransferCommand &command) = 0;
    virtual void UploadImage(const uint8_t *data, uint32_t sizeBytes) = 0;

    virtual void Flush() = 0;
    virtual void TextureFlush() = 0;
    virtual void Sync(GSSyncReason reason) = 0;
    virtual PresentationFrame Present(const GSPresentationRequest &request) = 0;

    virtual bool ClearFramebuffer(const GSContext &context, uint32_t rgba) = 0;
    virtual uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) = 0;

    virtual uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const = 0;
    virtual void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) = 0;
    virtual void SnapshotVram(std::vector<uint8_t> &out) const = 0;
    virtual GSTransferSnapshot GetTransferSnapshot() const = 0;
};
