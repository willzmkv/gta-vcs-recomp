// Native VCS LDM model decoder for runtime RSL cell streaming (Phase 3B - Option C)
// Decodes ldm resources into vertex/index buffers for GPU upload via RHI.
// Ported from tools/mdlinfo.py (Python reference decoder).

#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <memory>
#include <functional>
#include <fstream>
#include <iostream>

namespace vcs::ldm
{

// PS2 VU vertex formats (from mdlinfo.py decode)
enum class Vnvl : uint8_t
{
    Position3_16 = 0x9,  // V3-16: 3x int16 (position)
    UV2_8      = 0x6,  // V2-8: 2x uint8 (texture coordinates)
    Colour4_5  = 0xF,  // V4-5: 4x 5-bit (colour)
    Normal3_8  = 0xA,  // V3-8: 3x int8 (normal)
    Skin4_32   = 0xC   // V4-32: 4x float32 (skin weights)
};

// Attribute bitflags for vertex layout
enum class AttrFlag : uint32_t
{
    Position = 1u << 0,
    UV       = 1u << 1,
    Colour   = 1u << 2,
    Normal   = 1u << 3,
    Skin     = 1u << 4
};

// Vertex structure matching decoded attributes
struct Vertex
{
    float pos[3] = {0, 0, 0};
    float uv[2] = {0, 0};
    uint8_t colour[4] = {255, 255, 255, 255};
    float normal[3] = {0, 0, 0};
    float skin_weights[4] = {0, 0, 0, 0};
    uint16_t skin_indices[4] = {0, 0, 0, 0};
};

// Decoded mesh batch
struct MeshBatch
{
    uint32_t material_idx = 0;
    uint32_t triangle_count = 0;
    float uv_scale[2] = {1.0f, 1.0f};
    std::vector<uint32_t> indices;  // triangle list
    std::vector<Vertex> vertices;
    uint32_t attr_flags = 0;  // AttrFlag bitmask
};

// Decoded geometry (one element's geometry)
struct Geometry
{
    uint32_t num_verts = 0;
    int32_t flags = 0;
    float scale[3] = {1, 1, 1};
    float pos[3] = {0, 0, 0};
    std::vector<MeshBatch> meshes;
    std::vector<std::string> material_textures;
    std::vector<std::array<uint8_t, 4>> material_colours;
};

// Node in the model hierarchy
struct Node
{
    uint32_t offset = 0;
    std::string name;
    float pos[3] = {0, 0, 0};
    float matrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};  // modelling matrix
    float ltm[20] = {};  // LTM (local-to-world matrix)
    uint32_t child = 0, next = 0, root = 0;
    uint32_t id = 0, hierarchy = 0, hierarchy_id = 0;
    std::vector<uint32_t> children;
};

// Element (geometry attached to a node)
struct Element
{
    std::string node_name;
    Geometry geometry;
};

// Material info
struct Material
{
    std::string texture_name;
    std::array<uint8_t, 4> colour = {255, 255, 255, 255};
    bool has_matfx = false;
};

// Complete decoded model
struct Model
{
    std::vector<Node> nodes;
    std::vector<Element> elements;
    std::vector<Material> materials;
    uint32_t file_size = 0;
    uint32_t data_size = 0;
    uint32_t num_relocs = 0;
    uint32_t num_classes = 0;
    uint32_t num_funcs = 0;
};

// LDM resource header (0x20 bytes)
struct Header
{
    uint32_t ident = 0;        // 'ldm\0' = 0x006D646C
    uint32_t shrink = 0;
    uint32_t file_size = 0;
    uint32_t data_size = 0;
    uint32_t reloc_tab = 0;
    uint32_t num_relocs = 0;
    uint32_t global_tab = 0;
    uint16_t num_classes = 0;
    uint16_t num_funcs = 0;
};

// Main decoder class
class Decoder
{
public:
    // Decode from raw ldm bytes (as loaded from ISO/IMG)
    // Returns nullptr on parse failure
    static std::unique_ptr<Model> decode(const uint8_t* data, size_t size);

    // Decode from file path (loads file first)
    static std::unique_ptr<Model> decodeFile(const std::string& path);

    // Create GPU-ready buffers from decoded model
    // Fills vertex/index buffers for RHI upload
    struct GpuBuffers
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<MeshBatch> batches;  // per-material draw calls
        std::vector<std::string> textures;  // texture names to load
    };

    static GpuBuffers createGpuBuffers(const Model& model);

private:
    Decoder(const uint8_t* data, size_t size);

    // Pointer resolution with relocation table
    uint32_t readU32(size_t offset) const;
    uint32_t ptr(size_t offset) const;
    std::string cstr(size_t offset) const;

    // Decoding stages
    void decodeNodes(Model& model, size_t root_offset);
    void decodeElements(Model& model, size_t group_offset);
    void decodeGeometry(Geometry& geom, size_t geom_offset);
    void decodeMeshBatch(MeshBatch& batch, const Geometry& geom, size_t mesh_offset,
                         const float* scale, const float* pos, const float* uv_scale);

    // PS2 DMA decode (from mdlinfo.py)
    void decodeDmaBatch(MeshBatch& batch, size_t dma_offset, const float* scale,
                        const float* pos, const float* uv_scale);

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    std::unordered_set<size_t> relocs_;
    std::unordered_map<size_t, std::string> string_cache_;
};

} // namespace vcs::ldm