// Native VCS LDM model decoder for runtime RSL cell streaming (Phase 3B - Option C)
// Decodes ldm resources into vertex/index buffers for GPU upload via RHI.
// Ported from tools/mdlinfo.py (Python reference decoder).

#include "game/vcs_ldm.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace vcs::ldm
{

constexpr uint32_t MDL_IDENT = 0x006D646C;  // 'ldm\0'

// PS2 VU vertex formats (from mdlinfo.py decode)
static inline int vnvlSize(uint8_t vnvl)
{
    switch (vnvl)
    {
        case 0x9: return 6;   // V3-16: 3x int16
        case 0x6: return 2;   // V2-8: 2x uint8
        case 0xF: return 2;   // V4-5: packed
        case 0xA: return 2;   // V3-8
        case 0xC: return 16;  // V4-32: 4x float32
        default: return 0;
    }
}

Decoder::Decoder(const uint8_t* data, size_t size)
    : data_(data), size_(size)
{
    if (size < sizeof(Header))
        throw std::runtime_error("ldm: data too small for header");

    const Header* hdr = reinterpret_cast<const Header*>(data_);
    if (hdr->ident != MDL_IDENT)
        throw std::runtime_error("ldm: invalid ident");

    // Build relocation set
    relocs_.clear();
    for (uint32_t i = 0; i < hdr->num_relocs; ++i)
    {
        size_t reloc_off = hdr->reloc_tab + i * 4;
        if (reloc_off + 4 <= size_)
        {
            relocs_.insert(readU32(reloc_off));
        }
    }
}

uint32_t Decoder::readU32(size_t offset) const
{
    if (offset + 4 > size_)
        return 0;
    uint32_t val;
    std::memcpy(&val, data_ + offset, 4);
    return val;
}

uint32_t Decoder::ptr(size_t offset) const
{
    return relocs_.count(offset) ? readU32(offset) : 0;
}

std::string Decoder::cstr(size_t offset) const
{
    if (offset == 0 || offset >= size_)
        return "";
    auto it = string_cache_.find(offset);
    if (it != string_cache_.end())
        return it->second;

    const char* start = reinterpret_cast<const char*>(data_ + offset);
    const char* end = start;
    while (end < reinterpret_cast<const char*>(data_ + size_) && *end != '\0')
        ++end;
    std::string result(start, end - start);
    // Can't modify const map in const method, so we don't cache
    return result;
}

void Decoder::decodeNodes(Model& model, size_t root_offset)
{
    std::function<void(size_t, int)> collect = [&](size_t at, int depth)
    {
        if (!at || at >= size_)
            return;

        Node node;
        node.offset = at;
        node.name = cstr(ptr(at + 0xA8));
        float pos[3];
        std::memcpy(pos, data_ + at + 0x10 + 48, 12);
        std::memcpy(node.pos, pos, 12);

        // Modelling matrix at +0x10 (16 floats = 64 bytes)
        std::memcpy(node.matrix, data_ + at + 0x10, 64);

        // LTM at +0x50 (20 floats = 80 bytes)
        std::memcpy(node.ltm, data_ + at + 0x50, 80);

        node.child = ptr(at + 0x90);
        node.next = ptr(at + 0x94);
        node.root = ptr(at + 0x98);
        node.id = readU32(at + 0xA0);
        node.hierarchy = readU32(at + 0xA4);
        node.hierarchy_id = readU32(at + 0xA8);

        model.nodes.push_back(std::move(node));

        // Recurse children
        size_t child = ptr(at + 0x90);
        while (child && child < size_)
        {
            collect(child, depth + 1);
            child = ptr(child + 0x94);
        }
    };

    collect(root_offset, 0);
}

void Decoder::decodeElements(Model& model, size_t group_offset)
{
    if (!group_offset || group_offset >= size_)
        return;

    uint8_t type = data_[group_offset];
    bool single = (type == 1);

    size_t element_offset = single ? group_offset : ptr(group_offset + 8);
    size_t head = single ? group_offset : group_offset + 8;
    size_t link = ptr(head);

    std::unordered_set<size_t> seen;
    while (link && link != head && seen.insert(link).second)
    {
        size_t elem_offset = link - 0x1C;

        Element elem;
        size_t node_ptr = ptr(elem_offset + 4);
        // Find node name by offset
        for (const auto& n : model.nodes)
        {
            if (n.offset == node_ptr)
            {
                elem.node_name = n.name;
                break;
            }
        }

        size_t geom_offset = ptr(elem_offset + 0x14);
        if (geom_offset)
        {
            decodeGeometry(elem.geometry, geom_offset);
        }

        model.elements.push_back(std::move(elem));

        link = ptr(link);
    }
}

void Decoder::decodeGeometry(Geometry& geom, size_t geom_offset)
{
    if (geom_offset + 32 >= size_)
        return;

    // Material list at +0x0C, count at +0x10
    size_t mats_ptr = ptr(geom_offset + 0x0C);
    uint32_t mat_count = readU32(geom_offset + 0x10);
    geom.material_textures.resize(mat_count);
    geom.material_colours.resize(mat_count);

    for (uint32_t i = 0; i < mat_count; ++i)
    {
        size_t m = ptr(mats_ptr + i * 4);
        if (m)
        {
            geom.material_textures[i] = cstr(ptr(m));
            std::array<uint8_t, 4> col;
            std::memcpy(col.data(), data_ + m + 4, 4);
            geom.material_colours[i] = col;
        }
    }

    // PS2 geometry header at geom + 32
    size_t h = geom_offset + 32;
    if (h + 16 >= size_)
        return;

    geom.num_verts = readU32(h + 16);
    geom.flags = static_cast<int32_t>(readU32(h + 20));
    uint16_t v1 = readU32(h + 24) & 0xFFFF;
    uint16_t v2 = (readU32(h + 24) >> 16) & 0xFFFF;
    geom.num_verts = v1;
    geom.flags = static_cast<int32_t>(v2);

    size_t dma_off = readU32(h + 28);
    std::memcpy(geom.scale, data_ + h + 0x28, 12);
    std::memcpy(geom.pos, data_ + h + 0x34, 12);

    uint32_t n_meshes = geom.num_verts >> 20;
    size_t meshes_at = h + 0x40;
    size_t dma_base = meshes_at + n_meshes * 0x30;

    for (uint32_t i = 0; i < n_meshes; ++i)
    {
        size_t m = meshes_at + i * 0x30;
        float uv_scale[2];
        std::memcpy(uv_scale, data_ + m + 16, 8);

        uint32_t packet = readU32(m + 28);
        uint16_t tris = readU32(m + 30) & 0xFFFF;
        int16_t mat = static_cast<int16_t>(readU32(m + 32) & 0xFFFF);

        MeshBatch batch;
        batch.material_idx = (mat >= 0 && static_cast<uint32_t>(mat) < geom.material_textures.size()) ? mat : 0;
        batch.triangle_count = tris;
        batch.uv_scale[0] = uv_scale[0];
        batch.uv_scale[1] = uv_scale[1];

        decodeDmaBatch(batch, dma_base + packet, geom.scale, geom.pos, uv_scale);
        geom.meshes.push_back(std::move(batch));
    }
}

void Decoder::decodeDmaBatch(MeshBatch& batch, size_t dma_offset, const float* scale,
                             const float* pos, const float* uv_scale)
{
    if (dma_offset >= size_)
        return;

    // DMA data: qword count in first word
    uint32_t qwords = (readU32(dma_offset) & 0xFFFF) + 1;
    size_t end = dma_offset + qwords * 16;
    size_t w = dma_offset + 16;

    std::vector<Vertex> verts;
    std::vector<uint32_t> faces;
    std::unordered_set<uint8_t> kinds;
    bool first_batch = true;

    while (w < end)
    {
        uint32_t code = readU32(w);
        if (code != 0x6C018000)  // UNPACK strip header
            break;

        uint32_t nvert = readU32(w + 16) & 0x7FFF;
        w += 20;

        std::unordered_map<uint8_t, size_t> batch_data;

        while (true)
        {
            uint32_t code = readU32(w);
            uint8_t cmd = code >> 24;

            if (cmd == 0x14)  // MSCAL: batch ends
            {
                w += 4;
                while (w < end && readU32(w) == 0)
                    w += 4;
                break;
            }
            if (cmd == 0x20)  // STMASK
            {
                w += 8;
                continue;
            }
            if (cmd == 0x30)  // STROW
            {
                w += 20;
                continue;
            }
            if ((cmd & 0x60) != 0x60)  // not an UNPACK
            {
                w = end;
                break;
            }

            uint8_t vnvl = cmd & 0x0F;
            uint32_t num = (code >> 16) & 0xFF;
            int size = vnvlSize(vnvl);
            if (size == 0)
            {
                w = end;
                break;
            }

            batch_data[vnvl] = w + 4;
            kinds.insert(vnvl);
            w += 4 + ((num * size + 3) / 4) * 4;
        }

        int skip = first_batch ? 0 : 2;
        size_t p = batch_data.count(0x9) ? batch_data[0x9] : 0;
        size_t t = batch_data.count(0x6) ? batch_data[0x6] : 0;
        size_t base = verts.size();

        for (uint32_t i = skip; i < nvert; ++i)
        {
            Vertex v = {};
            if (p)
            {
                int16_t x, y, z;
                std::memcpy(&x, data_ + p + i * 6, 2);
                std::memcpy(&y, data_ + p + i * 6 + 2, 2);
                std::memcpy(&z, data_ + p + i * 6 + 4, 2);
                v.pos[0] = x / 32767.5f * scale[0] + pos[0];
                v.pos[1] = y / 32767.5f * scale[1] + pos[1];
                v.pos[2] = z / 32767.5f * scale[2] + pos[2];
            }
            if (t)
            {
                uint8_t u_val, v_val;
                std::memcpy(&u_val, data_ + t + i * 2, 1);
                std::memcpy(&v_val, data_ + t + i * 2 + 1, 1);
                v.uv[0] = u_val / 127.5f * uv_scale[0];
                v.uv[1] = v_val / 127.5f * uv_scale[1];
            }
            verts.push_back(v);
        }

        // Strip -> triangles (with two carried over vertices)
        std::vector<uint32_t> strip;
        for (size_t i = base - (first_batch ? 0 : 2); i < verts.size(); ++i)
            strip.push_back(static_cast<uint32_t>(i));

        for (size_t k = 2; k < strip.size(); ++k)
        {
            uint32_t a = strip[k - 2], b = strip[k - 1], c = strip[k];
            if (a != b && b != c && a != c)
            {
                if (k % 2 == 0)
                {
                    faces.push_back(a);
                    faces.push_back(b);
                    faces.push_back(c);
                }
                else
                {
                    faces.push_back(b);
                    faces.push_back(a);
                    faces.push_back(c);
                }
            }
        }

        first_batch = false;
    }

    batch.vertices = std::move(verts);
    batch.indices = std::move(faces);

    // Set attribute flags
    for (uint8_t k : kinds)
    {
        switch (k)
        {
            case 0x9: batch.attr_flags |= static_cast<uint32_t>(AttrFlag::Position); break;
            case 0x6: batch.attr_flags |= static_cast<uint32_t>(AttrFlag::UV); break;
            case 0xF: batch.attr_flags |= static_cast<uint32_t>(AttrFlag::Colour); break;
            case 0xA: batch.attr_flags |= static_cast<uint32_t>(AttrFlag::Normal); break;
            case 0xC: batch.attr_flags |= static_cast<uint32_t>(AttrFlag::Skin); break;
        }
    }
}

// ---- Public API ----

std::unique_ptr<Model> Decoder::decode(const uint8_t* data, size_t size)
{
    try
    {
        Decoder decoder(data, size);
        auto model = std::make_unique<Model>();

        const Header* hdr = reinterpret_cast<const Header*>(data);
        model->file_size = hdr->file_size;
        model->data_size = hdr->data_size;
        model->num_relocs = hdr->num_relocs;
        model->num_classes = hdr->num_classes;
        model->num_funcs = hdr->num_funcs;

        // Vehicle model starts at 0x20
        size_t group_offset = decoder.ptr(0x20);
        if (!group_offset)
            group_offset = 0x20;

        if (group_offset && group_offset < size)
        {
            decoder.decodeElements(*model, group_offset);
        }

        size_t root_offset = decoder.ptr(group_offset + 4);
        if (root_offset)
        {
            decoder.decodeNodes(*model, root_offset);
        }

        return model;
    }
    catch (const std::exception& e)
    {
        std::cerr << "ldm decode error: " << e.what() << std::endl;
        return nullptr;
    }
}

Decoder::GpuBuffers Decoder::createGpuBuffers(const Model& model)
{
    GpuBuffers gpu;

    // Collect all vertices and indices, merge by material
    std::unordered_map<uint32_t, MeshBatch> material_batches;

    for (const auto& elem : model.elements)
    {
        for (const auto& geom_batch : elem.geometry.meshes)
        {
            auto& merged = material_batches[geom_batch.material_idx];
            if (merged.vertices.empty())
            {
                merged = geom_batch;
            }
            else
            {
                // Merge vertices/indices (would need index remapping for real use)
                size_t offset = merged.vertices.size();
                merged.vertices.insert(merged.vertices.end(),
                                       geom_batch.vertices.begin(), geom_batch.vertices.end());
                for (uint32_t idx : geom_batch.indices)
                    merged.indices.push_back(offset + idx);
            }
        }
    }

    for (auto& [mat_idx, batch] : material_batches)
    {
        gpu.batches.push_back(std::move(batch));
    }

    // Collect texture names
    for (const auto& elem : model.elements)
    {
        for (const auto& mat : elem.geometry.material_textures)
        {
            if (!mat.empty())
                gpu.textures.push_back(mat);
        }
    }

    // Deduplicate textures
    std::sort(gpu.textures.begin(), gpu.textures.end());
    gpu.textures.erase(std::unique(gpu.textures.begin(), gpu.textures.end()), gpu.textures.end());

    return gpu;
}

} // namespace vcs::ldm