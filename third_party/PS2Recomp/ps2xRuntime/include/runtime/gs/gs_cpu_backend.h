#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_texture_page_cache.h"

#include <array>
#include <mutex>
#include <vector>

class GSCpuBackend final : public GSRasterBackend
{
public:
    GSCpuBackend();

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

    // [gta-recomp] For the hardware renderer: the whole texture of `state` (TEX0/TEXA/current CLUT) as
    // RGBA8 (PS2 values, 0x80 = 1.0), width x height = state.textureWidth x textureHeight.
    void DecodeTexture(const GSDrawState &state, std::vector<uint32_t> &out);
    // [gta-recomp] Snapshot CLUT palette + stateless buffer decoder so helper threads can decode VRAM snapshots (#113).
    bool ComputePaletteTable(const GSDrawState &state, uint32_t (&table)[256]);
    static bool DecodeTextureBuffer(const GSDrawState &state, const uint8_t *vram, size_t vramBytes, bool wrapVram,
                                    bool hasPalette, const uint32_t (&palette)[256], std::vector<uint32_t> &out);
    // [gta-recomp] The same from texels already fetched (raw values as a VRAM read returns them, width x height, row by
    // row) - docs/FULLY_NATIVE.md step 3c: a texture decoded from the upload that wrote it
    void DecodeTextureFromTexels(const GSDrawState &state, const std::vector<uint32_t> &texels, std::vector<uint32_t> &out);
    uint64_t ClutHash(const GSTex0Reg &tex0) const;
    // [gta-recomp] A CLUT load (TEX0.CLD rules as LoadClut) whose source pixels come from a linear rectangle - the data of
    // the upload that wrote them (rect: rectW x rectH pixels of 2 or 4 bytes from the CLUT buffer's origin) - instead of
    // VRAM. -1 when a source pixel is outside the rectangle (nothing changed then), 0 when CLD skips the load, 1 loaded.
    int LoadClutFromRect(const GSTex0Reg &tex0, const GSTexClutReg &texclut, const uint8_t *rect, uint32_t rectW, uint32_t rectH);
    // [gta-recomp] PS2X_HW_CLUT_VERIFY: the CLUT entries, and a CLUT load from VRAM ignoring CLD
    const std::array<uint16_t, 512> &ClutEntries() const { return m_clut; }
    void ReloadClutFromVram(const GSTex0Reg &tex0, const GSTexClutReg &texclut);

private:
    void ResetUnlocked();
    void LoadClutUnlocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut);
    bool clutLoadWanted(const GSTex0Reg &tex0); // TEX0.CLD: whether this TEX0 loads the CLUT (updates CBP0 / CBP1)
    template <bool Write, typename Read>
    bool loadClutEntries(const GSTex0Reg &tex0, const GSTexClutReg &texclut, Read read);
    uint32_t ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;
    uint32_t ReadTextureVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y);
    void WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);

    void DrawPrimitive(const GSPrimitiveBatch &batch);
    void DrawSprite(const GSPrimitiveBatch &batch);
    void DrawTriangle(const GSPrimitiveBatch &batch);
    void DrawLine(const GSPrimitiveBatch &batch);
    void WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog);
    uint32_t SampleTexture(const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v);
    uint32_t LookupCLUT(const GSDrawState &state, uint8_t index, uint8_t cpsm, uint8_t csm, uint8_t csa, uint8_t sourcePsm);
    uint32_t convertTexel(const GSDrawState &state, uint32_t raw); // [gta-recomp] DecodeTexture's per-texel step
    bool paletteTable(const GSDrawState &state, uint32_t (&table)[256]); // [gta-recomp] convertTexel for every index

    void PerformLocalToLocalTransfer();
    void PerformLocalToHostTransfer();
    PresentationFrame PresentFromLocalMemory(const GSPresentationRequest &request);
    bool CopyFrameToHostRgba(const GSFrameReg &frame,
                             uint32_t width,
                             uint32_t height,
                             std::vector<uint8_t> &outPixels,
                             bool preserveAlpha,
                             bool useLocalMemoryLayout,
                             bool frameBaseIsPages,
                             uint32_t sourceOriginX,
                             uint32_t sourceOriginY) const;

    using WriteVramFunc = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    using ReadVramFunc = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);

    static constexpr size_t kPsmHandlerCount = 1u << 6u;
    mutable std::mutex m_mutex;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;
    std::array<ReadVramFunc, kPsmHandlerCount> m_readVramFuncs{};
    std::array<WriteVramFunc, kPsmHandlerCount> m_writeVramFuncs{};
    std::array<uint16_t, 512> m_clut{};
    mutable uint64_t m_clutHashCache = 0;   // [gta-recomp] ClutHash of m_clut, valid unless m_clutHashDirty
    mutable bool m_clutHashDirty = true;
    std::array<uint32_t, 2> m_clutCbp{};
    GSMem::TexturePageCache m_texturePageCache;

    GSTransferCommand m_transfer{};
    GSTransferSnapshot m_transferState{};
    std::vector<uint8_t> m_localToHostBuffer;
    size_t m_localToHostReadPos = 0;
};
