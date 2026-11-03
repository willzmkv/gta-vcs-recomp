// tools/dff2mdl.py and tools/txd2xtx.py in C++ (rwconv.h). Every step mirrors the Python's - the same order, the same
// doubles, Python's rounding (half to even), its dictionaries' insertion order - so both write the same bytes
// (tools/rwconv_check.py). A change here is a change there.
#include "mods/rwconv.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(__clang__)
#pragma clang fp contract(off) // a * b + c as two roundings, as Python computes it
#endif

namespace vcs::rwconv
{
    namespace
    {
        using Bytes = std::vector<uint8_t>;
        using V3 = std::array<double, 3>;
        using V2 = std::array<double, 2>;

        struct Bad : std::runtime_error
        {
            using std::runtime_error::runtime_error;
        };

        uint32_t u32(const Bytes &d, uint64_t at)
        {
            if (at + 4 > d.size())
                throw Bad("a read past the end of the file");
            uint32_t v;
            std::memcpy(&v, d.data() + at, 4);
            return v;
        }

        uint16_t u16(const Bytes &d, uint64_t at)
        {
            if (at + 2 > d.size())
                throw Bad("a read past the end of the file");
            uint16_t v;
            std::memcpy(&v, d.data() + at, 2);
            return v;
        }

        uint8_t u8(const Bytes &d, uint64_t at)
        {
            if (at >= d.size())
                throw Bad("a read past the end of the file");
            return d[static_cast<size_t>(at)];
        }

        double f32(const Bytes &d, uint64_t at)
        {
            const uint32_t bits = u32(d, at);
            float f;
            std::memcpy(&f, &bits, 4);
            return f;
        }

        // a .dff's float: NaN / infinity (some exporters write them) read as 0 (dff2mdl.finite)
        double finite32(const Bytes &d, uint64_t at)
        {
            const double v = f32(d, at);
            return std::isfinite(v) ? v : 0.0;
        }

        void put32(Bytes &d, uint64_t at, uint32_t v)
        {
            if (at + 4 > d.size())
                throw Bad("a write past the end");
            std::memcpy(d.data() + at, &v, 4);
        }

        void putf(Bytes &d, uint64_t at, double v)
        {
            const float f = static_cast<float>(v);
            uint32_t bits;
            std::memcpy(&bits, &f, 4);
            put32(d, at, bits);
        }

        void add32(Bytes &d, uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
                d.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }

        void add16(Bytes &d, uint32_t v)
        {
            d.push_back(static_cast<uint8_t>(v));
            d.push_back(static_cast<uint8_t>(v >> 8));
        }

        void addf(Bytes &d, double v)
        {
            const float f = static_cast<float>(v);
            uint32_t bits;
            std::memcpy(&bits, &f, 4);
            add32(d, bits);
        }

        void padTo(Bytes &d, size_t n)
        {
            while (d.size() % n)
                d.push_back(0);
        }

        // Python's int(round(x)): to nearest, halves to even
        int64_t pyRound(double x)
        {
            const double f = std::floor(x), diff = x - f;
            const auto fi = static_cast<int64_t>(f);
            if (diff > 0.5)
                return fi + 1;
            if (diff < 0.5)
                return fi;
            return fi % 2 == 0 ? fi : fi + 1;
        }

        int64_t clampI(int64_t v, int64_t lo, int64_t hi) { return std::max(lo, std::min(hi, v)); }

        // Python's str.lower() on a Latin-1 string
        std::string lower(std::string s)
        {
            for (char &c : s)
            {
                const auto u = static_cast<unsigned char>(c);
                if ((u >= 'A' && u <= 'Z') || (u >= 0xC0 && u <= 0xDE && u != 0xD7))
                    c = static_cast<char>(u + 0x20);
            }
            return s;
        }

        std::string cString(const Bytes &d, uint64_t at, uint64_t end)
        {
            std::string s;
            for (uint64_t i = at; i < end && i < d.size() && d[static_cast<size_t>(i)]; ++i)
                s.push_back(static_cast<char>(d[static_cast<size_t>(i)]));
            return s;
        }

        // ---- RenderWare binary streams (tools/rwinfo.py) ------------------------------------------------------------
        constexpr uint32_t kStruct = 0x01, kString = 0x02, kExtension = 0x03, kTexture = 0x06, kMaterial = 0x07, kMatList = 0x08,
                           kFrameList = 0x0E, kGeometry = 0x0F, kClump = 0x10, kAtomic = 0x14, kTexNative = 0x15, kTexDict = 0x16,
                           kGeomList = 0x1A, kFrameName = 0x0253F2FEu, kBinMesh = 0x50E;

        struct Chunk
        {
            uint32_t kind;
            uint64_t at, end;
            uint32_t libid;
        };

        std::vector<Chunk> chunks(const Bytes &d, uint64_t at, uint64_t end)
        {
            std::vector<Chunk> out;
            while (at + 12 <= end)
            {
                const uint32_t kind = u32(d, at), size = u32(d, at + 4), libid = u32(d, at + 8);
                out.push_back({kind, at + 12, std::min<uint64_t>(at + 12 + size, end), libid});
                at += 12 + uint64_t(size);
            }
            return out;
        }

        std::optional<std::pair<uint64_t, uint64_t>> child(const Bytes &d, uint64_t at, uint64_t end, uint32_t kind)
        {
            for (const Chunk &c : chunks(d, at, end))
                if (c.kind == kind)
                    return std::make_pair(c.at, c.end);
            return std::nullopt;
        }

        std::pair<uint64_t, uint64_t> need(const std::optional<std::pair<uint64_t, uint64_t>> &c, const char *what)
        {
            if (!c)
                throw Bad(std::string("no ") + what + " chunk");
            return *c;
        }

        uint32_t version(uint32_t libid)
        {
            if (libid & 0xFFFF0000u)
                return (((libid >> 14) & 0x3FF00u) + 0x30000u) | ((libid >> 16) & 0x3Fu);
            return libid << 8;
        }

        std::string rwString(const Bytes &d, uint64_t at, uint64_t end)
        {
            const auto s = child(d, at, end, kString);
            return s ? cString(d, s->first, s->second) : std::string();
        }

        // ---- the PC .dff (dff2mdl.read_dff) ---------------------------------------------------------------------------
        struct Frame
        {
            double rot[9], pos[3];
            int32_t parent;
            std::string name;
        };
        struct Material
        {
            uint8_t rgba[4];
            std::string texture;
        };
        struct Tri
        {
            uint32_t a, b, c, mat;
        };
        struct Geometry
        {
            std::vector<V3> verts, normals; // normals empty: none
            std::vector<V2> uvs;
            std::vector<Tri> tris;
            std::vector<Material> materials;
        };
        struct Atomic
        {
            uint32_t frame, geometry;
        };
        struct Dff
        {
            std::vector<Frame> frames;
            std::vector<Geometry> geometries;
            std::vector<Atomic> atomics;
        };

        Dff readDff(const Bytes &d)
        {
            const auto all = chunks(d, 0, d.size());
            if (all.empty() || all[0].kind != kClump)
                throw Bad("the .dff is not a RenderWare clump");
            Dff out;
            for (const Chunk &c : chunks(d, all[0].at, all[0].end))
            {
                if (c.kind == kFrameList)
                {
                    const auto s = need(child(d, c.at, c.end, kStruct), "frame list struct");
                    const uint32_t n = u32(d, s.first);
                    for (uint32_t i = 0; i < n; ++i)
                    {
                        Frame f{};
                        const uint64_t at = s.first + 4 + uint64_t(i) * 56;
                        for (int k = 0; k < 9; ++k)
                            f.rot[k] = finite32(d, at + 4 * k);
                        for (int k = 0; k < 3; ++k)
                            f.pos[k] = finite32(d, at + 36 + 4 * k);
                        f.parent = static_cast<int32_t>(u32(d, at + 48));
                        out.frames.push_back(f);
                    }
                    uint32_t i = 0;
                    for (const Chunk &x : chunks(d, s.second, c.end))
                        if (x.kind == kExtension && i < n)
                        {
                            for (const Chunk &p : chunks(d, x.at, x.end))
                                if (p.kind == kFrameName)
                                    out.frames[i].name.assign(d.begin() + static_cast<ptrdiff_t>(p.at), d.begin() + static_cast<ptrdiff_t>(p.end));
                            ++i;
                        }
                }
                else if (c.kind == kGeomList)
                {
                    for (const Chunk &gc : chunks(d, c.at, c.end))
                    {
                        if (gc.kind != kGeometry)
                            continue;
                        Geometry g;
                        const auto s = need(child(d, gc.at, gc.end, kStruct), "geometry struct");
                        uint64_t at = s.first;
                        const uint32_t flags = u32(d, at), ntris = u32(d, at + 4), nverts = u32(d, at + 8);
                        at += 16;
                        if (version(gc.libid) < 0x34000)
                            at += 12;
                        if (flags & 0x08) // prelit colours
                            at += uint64_t(nverts) * 4;
                        uint32_t nsets = (flags >> 16) & 0xFF;
                        if (!nsets)
                            nsets = (flags & 0x80) ? 2 : (flags & 0x04) ? 1 : 0;
                        for (uint32_t i = 0; i < nverts; ++i)
                            g.uvs.push_back(nsets ? V2{finite32(d, at + i * 8ull), finite32(d, at + i * 8ull + 4)} : V2{0.0, 0.0});
                        at += uint64_t(nsets) * nverts * 8;
                        for (uint32_t i = 0; i < ntris; ++i)
                        {
                            const uint32_t w0 = u32(d, at + i * 8ull), w1 = u32(d, at + i * 8ull + 4);
                            g.tris.push_back({w0 >> 16, w0 & 0xFFFF, w1 >> 16, w1 & 0xFFFF});
                        }
                        at += uint64_t(ntris) * 8 + 16; // + the morph target's bounding sphere
                        const uint32_t hasNormals = u32(d, at + 4);
                        at += 8;
                        for (uint32_t i = 0; i < nverts; ++i)
                            g.verts.push_back({finite32(d, at + i * 12ull), finite32(d, at + i * 12ull + 4), finite32(d, at + i * 12ull + 8)});
                        at += uint64_t(nverts) * 12;
                        if (hasNormals)
                            for (uint32_t i = 0; i < nverts; ++i)
                                g.normals.push_back({finite32(d, at + i * 12ull), finite32(d, at + i * 12ull + 4), finite32(d, at + i * 12ull + 8)});
                        std::vector<Material> own;
                        const auto ml = child(d, gc.at, gc.end, kMatList);
                        if (ml)
                        {
                            for (const Chunk &mc : chunks(d, ml->first, ml->second))
                                if (mc.kind == kMaterial)
                                {
                                    Material m{};
                                    const uint64_t ms = need(child(d, mc.at, mc.end, kStruct), "material struct").first;
                                    for (int k = 0; k < 4; ++k)
                                        m.rgba[k] = u8(d, ms + 4 + k);
                                    if (const auto tex = child(d, mc.at, mc.end, kTexture))
                                        m.texture = rwString(d, tex->first, tex->second);
                                    own.push_back(m);
                                }
                            const uint64_t ls = need(child(d, ml->first, ml->second, kStruct), "material list struct").first;
                            const uint32_t count = u32(d, ls);
                            size_t k = 0;
                            for (uint32_t i = 0; i < count; ++i)
                            {
                                const auto ref = static_cast<int32_t>(u32(d, ls + 4 + i * 4ull));
                                if (ref < 0)
                                {
                                    g.materials.push_back(k < own.size() ? own[k] : Material{{255, 255, 255, 255}, ""});
                                    ++k;
                                }
                                else
                                {
                                    if (static_cast<size_t>(ref) >= g.materials.size())
                                        throw Bad("a material list entry refers past itself");
                                    g.materials.push_back(g.materials[static_cast<size_t>(ref)]);
                                }
                            }
                        }
                        // the bin mesh (what RW draws) splits by material; the triangles' own numbers are often all 0
                        if (const auto ext = child(d, gc.at, gc.end, kExtension))
                            if (const auto bm = child(d, ext->first, ext->second, kBinMesh))
                            {
                                const uint32_t bflags = u32(d, bm->first), nmesh = u32(d, bm->first + 4);
                                uint64_t p = bm->first + 12;
                                std::vector<Tri> fromMesh;
                                for (uint32_t m = 0; m < nmesh; ++m)
                                {
                                    const uint32_t cnt = u32(d, p), mat = u32(d, p + 4);
                                    std::vector<uint32_t> idx(cnt);
                                    for (uint32_t i = 0; i < cnt; ++i)
                                        idx[i] = u32(d, p + 8 + i * 4ull);
                                    p += 8 + 4ull * cnt;
                                    if (bflags & 1)
                                    {
                                        for (int64_t j = 0; j < int64_t(cnt) - 2; ++j)
                                        {
                                            const uint32_t a = idx[j], b = idx[j + 1], c3 = idx[j + 2];
                                            if (a != b && b != c3 && a != c3)
                                                fromMesh.push_back(j % 2 == 0 ? Tri{a, b, c3, mat} : Tri{b, a, c3, mat});
                                        }
                                    }
                                    else
                                        for (int64_t j = 0; j < int64_t(cnt) - 2; j += 3)
                                            fromMesh.push_back({idx[j], idx[j + 1], idx[j + 2], mat});
                                }
                                if (!fromMesh.empty())
                                    g.tris = std::move(fromMesh);
                            }
                        for (const Tri &t : g.tris)
                            if (t.a >= nverts || t.b >= nverts || t.c >= nverts)
                                throw Bad("a triangle refers past the vertices");
                        out.geometries.push_back(std::move(g));
                    }
                }
                else if (c.kind == kAtomic)
                {
                    const uint64_t s = need(child(d, c.at, c.end, kStruct), "atomic struct").first;
                    out.atomics.push_back({u32(d, s), u32(d, s + 4)});
                }
            }
            return out;
        }

        // ---- matrices (dff2mdl.mat_mul / mat_inv / mat_apply): RW's row vectors, v * r + p ------------------------------
        struct Mat
        {
            double r[9], p[3];
        };
        constexpr Mat kIdentity{{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}, {0.0, 0.0, 0.0}};

        Mat matMul(const Mat &a, const Mat &b) // a then b
        {
            Mat out{};
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    out.r[3 * i + j] = a.r[3 * i] * b.r[j] + a.r[3 * i + 1] * b.r[3 + j] + a.r[3 * i + 2] * b.r[6 + j];
            for (int j = 0; j < 3; ++j)
                out.p[j] = a.p[0] * b.r[j] + a.p[1] * b.r[3 + j] + a.p[2] * b.r[6 + j] + b.p[j];
            return out;
        }

        Mat matInv(const Mat &m)
        {
            const double a = m.r[0], b = m.r[1], c = m.r[2], d = m.r[3], e = m.r[4], f = m.r[5], g = m.r[6], h = m.r[7], i = m.r[8];
            const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
            if (std::fabs(det) < 1e-12)
                return kIdentity;
            const double cof[9] = {e * i - f * h, c * h - b * i, b * f - c * e, f * g - d * i, a * i - c * g,
                                   c * d - a * f, d * h - e * g, b * g - a * h, a * e - b * d};
            Mat out{};
            for (int k = 0; k < 9; ++k)
                out.r[k] = cof[k] / det;
            for (int j = 0; j < 3; ++j)
                out.p[j] = -(m.p[0] * out.r[j] + m.p[1] * out.r[3 + j] + m.p[2] * out.r[6 + j]);
            return out;
        }

        V3 matApply(const Mat &m, const V3 &v, bool translate = true)
        {
            V3 out;
            for (int j = 0; j < 3; ++j)
                out[j] = v[0] * m.r[j] + v[1] * m.r[3 + j] + v[2] * m.r[6 + j] + (translate ? m.p[j] : 0.0);
            return out;
        }

        // dff2mdl.uv_tiles: a material's triangles as meshes - one while its UVs span at most two repeats, else one per
        // triangle's repeat (its lowest whole u, v)
        std::vector<std::vector<std::array<uint32_t, 3>>> uvTiles(const std::vector<std::array<uint32_t, 3>> &tris, const std::vector<V2> &uvs)
        {
            if (tris.empty())
                return {tris};
            double minU = 0, maxU = 0, minV = 0, maxV = 0;
            bool first = true;
            for (const auto &t : tris)
                for (const uint32_t i : t)
                {
                    const V2 &uv = uvs.at(i);
                    minU = first ? uv[0] : std::min(minU, uv[0]), maxU = first ? uv[0] : std::max(maxU, uv[0]);
                    minV = first ? uv[1] : std::min(minV, uv[1]), maxV = first ? uv[1] : std::max(maxV, uv[1]);
                    first = false;
                }
            if (maxU - std::floor(minU) <= 2.0 && maxV - std::floor(minV) <= 2.0)
                return {tris};
            std::vector<std::pair<std::pair<double, double>, std::vector<std::array<uint32_t, 3>>>> tiles;
            for (const auto &t : tris)
            {
                const std::pair<double, double> tile{std::floor(std::min({uvs[t[0]][0], uvs[t[1]][0], uvs[t[2]][0]})),
                                                     std::floor(std::min({uvs[t[0]][1], uvs[t[1]][1], uvs[t[2]][1]}))};
                auto it = std::find_if(tiles.begin(), tiles.end(), [&](const auto &x) { return x.first == tile; });
                if (it == tiles.end())
                    tiles.push_back({tile, {t}});
                else
                    it->second.push_back(t);
            }
            std::vector<std::vector<std::array<uint32_t, 3>>> out;
            for (auto &x : tiles)
                out.push_back(std::move(x.second));
            return out;
        }

        // ---- strips and the PS2 mesh (dff2mdl.strip_of, encode) -----------------------------------------------------------
        // dff2mdl.strip_of: strips started at the triangle with the fewest free neighbours, grown along shared edges to
        // the neighbour with the fewest free neighbours, both ways, joined by repeating a vertex at each end
        std::vector<uint32_t> stripOf(const std::vector<std::array<uint32_t, 3>> &input)
        {
            std::vector<std::array<uint32_t, 3>> tris;
            for (const auto &t : input)
                if (t[0] != t[1] && t[1] != t[2] && t[0] != t[2])
                    tris.push_back(t);
            const size_t count = tris.size();
            auto key = [](uint32_t a, uint32_t b) { return (uint64_t(std::min(a, b)) << 32) | std::max(a, b); };
            std::unordered_map<uint64_t, std::vector<size_t>> byEdge;
            for (size_t t = 0; t < count; ++t)
            {
                const auto &[a, b, c] = tris[t];
                byEdge[key(a, b)].push_back(t);
                byEdge[key(b, c)].push_back(t);
                byEdge[key(c, a)].push_back(t);
            }
            std::vector<std::vector<size_t>> neighbours(count);
            for (size_t t = 0; t < count; ++t)
            {
                const auto &[a, b, c] = tris[t];
                for (const uint64_t e : {key(a, b), key(b, c), key(c, a)})
                    for (const size_t u : byEdge[e])
                        if (u != t && std::find(neighbours[t].begin(), neighbours[t].end(), u) == neighbours[t].end())
                            neighbours[t].push_back(u);
            }
            std::vector<char> used(count, 0);
            std::vector<int64_t> freeCount(count);
            using Entry = std::pair<int64_t, size_t>;
            std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap;
            for (size_t t = 0; t < count; ++t)
            {
                freeCount[t] = static_cast<int64_t>(neighbours[t].size());
                heap.push({freeCount[t], t});
            }
            auto grow = [&](std::vector<uint32_t> s, std::set<size_t> &taken) {
                for (int pass = 0; pass < 2; ++pass) // forward, then from the other end
                {
                    for (;;)
                    {
                        const auto it = byEdge.find(key(s[s.size() - 2], s[s.size() - 1]));
                        size_t best = SIZE_MAX;
                        if (it != byEdge.end())
                            for (const size_t u : it->second)
                                if (!used[u] && !taken.count(u) &&
                                    (best == SIZE_MAX || Entry{freeCount[u], u} < Entry{freeCount[best], best}))
                                    best = u;
                        if (best == SIZE_MAX)
                            break;
                        taken.insert(best);
                        const uint32_t x = s[s.size() - 2], y = s[s.size() - 1];
                        for (const uint32_t v : tris[best])
                            if (v != x && v != y)
                            {
                                s.push_back(v);
                                break;
                            }
                    }
                    std::reverse(s.begin(), s.end());
                }
                return s;
            };
            std::vector<uint32_t> out;
            while (!heap.empty())
            {
                const Entry top = heap.top();
                heap.pop();
                const size_t t = top.second;
                if (used[t] || top.first != freeCount[t])
                    continue; // taken, or an older entry for it
                const auto &[a, b, c] = tris[t];
                std::vector<uint32_t> best;
                std::set<size_t> bestTaken;
                bool have = false;
                for (const auto &start : {std::vector<uint32_t>{a, b, c}, std::vector<uint32_t>{b, c, a}, std::vector<uint32_t>{c, a, b}})
                {
                    std::set<size_t> taken{t};
                    std::vector<uint32_t> s = grow(start, taken);
                    if (!have || taken.size() > bestTaken.size())
                    {
                        best = std::move(s);
                        bestTaken = std::move(taken);
                        have = true;
                    }
                }
                for (const size_t k : bestTaken) // a std::set: in order, as sorted() in the Python
                    used[k] = 1;
                for (const size_t k : bestTaken)
                    for (const size_t u : neighbours[k])
                        if (!used[u])
                        {
                            --freeCount[u];
                            heap.push({freeCount[u], u});
                        }
                if (!out.empty())
                {
                    const uint32_t last = out.back();
                    out.push_back(last);
                    out.push_back(best[0]);
                }
                out.insert(out.end(), best.begin(), best.end());
            }
            return out;
        }

        struct Raw
        {
            int16_t p[3];
            uint8_t uv[2];
            int8_t n[3];
        };

        constexpr size_t kBatch = 70;

        Bytes encode(const std::vector<Raw> &verts)
        {
            Bytes body;
            size_t start = 0;
            for (;;)
            {
                const size_t n = std::min(kBatch, verts.size() - start);
                const auto nn = static_cast<uint32_t>(n);
                for (const uint32_t w : {0x6C018000u, 0u, 0u, nn, nn | 0x8000u})
                    add32(body, w);
                for (const uint32_t w : {0x20000000u, 0x40404040u, 0x30000000u, 0u, 0u, 0u, 0u})
                    add32(body, w);
                add32(body, 0x79008001u | nn << 16);
                for (size_t i = 0; i < n; ++i)
                    for (const int16_t c : verts[start + i].p)
                        add16(body, static_cast<uint16_t>(c));
                padTo(body, 4);
                for (const uint32_t w : {0x20000000u, 0x50505050u, 0x30000000u, 0u, 0u, 0xFFu, 0u})
                    add32(body, w);
                add32(body, 0x7600C08Du | nn << 16);
                for (size_t i = 0; i < n; ++i)
                {
                    body.push_back(verts[start + i].uv[0]);
                    body.push_back(verts[start + i].uv[1]);
                }
                padTo(body, 4);
                add32(body, 0x6A008047u | nn << 16);
                for (size_t i = 0; i < n; ++i)
                    for (const int8_t c : verts[start + i].n)
                        body.push_back(static_cast<uint8_t>(c));
                padTo(body, 4);
                add32(body, 0x14000006u);
                while ((16 + body.size()) % 16)
                    body.push_back(0);
                if (start + kBatch >= verts.size())
                    break;
                start += kBatch - 2;
            }
            Bytes out;
            for (const uint32_t w : {0x60000000u | static_cast<uint32_t>(body.size() / 16), 0u, 0u, 0u})
                add32(out, w);
            out.insert(out.end(), body.begin(), body.end());
            return out;
        }

        // ---- the VCS model (mdlinfo.Mdl) ------------------------------------------------------------------------------------
        struct Mdl
        {
            const Bytes &d;
            uint32_t ident, shrink, fileSize, dataSize, relocTab, numRelocs, globalTab;
            uint16_t numClasses, numFuncs;
            std::set<uint32_t> relocs;

            explicit Mdl(const Bytes &data) : d(data)
            {
                ident = u32(d, 0), shrink = u32(d, 4), fileSize = u32(d, 8), dataSize = u32(d, 12), relocTab = u32(d, 16),
                numRelocs = u32(d, 20), globalTab = u32(d, 24), numClasses = u16(d, 28), numFuncs = u16(d, 30);
                if (ident != 0x006D646Cu)
                    throw Bad("the template is not an ldm resource");
                if (dataSize > d.size() || fileSize > d.size() || globalTab > relocTab)
                    throw Bad("the template's header does not fit the file");
                for (uint32_t i = 0; i < numRelocs; ++i)
                    relocs.insert(u32(d, relocTab + i * 4ull));
            }
            uint32_t ptr(uint64_t at) const { return relocs.count(static_cast<uint32_t>(at)) ? u32(d, at) : 0u; }
            std::string cstr(uint32_t at) const
            {
                if (!at)
                    return {};
                for (uint64_t i = at; i < d.size(); ++i)
                    if (!d[static_cast<size_t>(i)])
                        return std::string(d.begin() + at, d.begin() + static_cast<ptrdiff_t>(i));
                throw Bad("a string runs past the end");
            }
            void nodes(uint32_t at, std::vector<std::pair<uint32_t, std::string>> &out, int depth = 0) const
            {
                if (depth > 64)
                    throw Bad("the node tree is too deep");
                out.push_back({at, cstr(ptr(at + 0xA8))});
                for (uint32_t c = ptr(at + 0x90); c; c = ptr(c + 0x94))
                    nodes(c, out, depth + 1);
            }
            std::vector<uint32_t> elements(uint32_t group) const
            {
                std::vector<uint32_t> out;
                std::set<uint32_t> seen;
                const uint32_t head = group + 8;
                for (uint32_t link = ptr(head); link && link != head && !seen.count(link); link = ptr(link))
                {
                    seen.insert(link);
                    out.push_back(link - 0x1C);
                }
                return out;
            }
        };

        // dff2mdl.build_geometry: {RslGeometry, PS2 header, meshes, DMA data, material pointers}; the relocations inside
        Bytes buildGeometry(const Mdl &m, uint32_t templateGeom, const std::vector<V3> &verts, const std::vector<V3> &normals,
                            const std::vector<V2> &uvs, const std::vector<std::pair<int, std::vector<uint32_t>>> &groups,
                            const std::vector<uint32_t> &materials, std::vector<uint32_t> &inner)
        {
            V3 lo{0, 0, 0}, hi{0, 0, 0};
            for (int k = 0; k < 3; ++k)
            {
                lo[k] = hi[k] = verts.empty() ? 0.0 : verts[0][k];
                for (const V3 &v : verts)
                    lo[k] = std::min(lo[k], v[k]), hi[k] = std::max(hi[k], v[k]);
            }
            V3 centre, scale;
            for (int k = 0; k < 3; ++k)
            {
                centre[k] = (lo[k] + hi[k]) / 2;
                scale[k] = std::max(1e-3, (hi[k] - lo[k]) / 2 * 1.001);
            }
            double radius = 0.0;
            bool first = true;
            for (const V3 &v : verts)
            {
                const double r = std::pow(std::pow(v[0] - centre[0], 2) + std::pow(v[1] - centre[1], 2) + std::pow(v[2] - centre[2], 2), 0.5);
                radius = first ? r : std::max(radius, r);
                first = false;
            }
            auto q16 = [](double x) { return static_cast<int16_t>(clampI(pyRound(x * 32767.5), -32767, 32767)); };

            Bytes meshes, dma;
            uint32_t total = 0;
            int gbox[6] = {0, 0, 0, 0, 0, 0};
            bool firstMesh = true;
            for (const auto &[material, strip] : groups)
            {
                double offU = 0, offV = 0, maxU = 0, maxV = 0;
                for (size_t i = 0; i < strip.size(); ++i)
                {
                    const V2 &u = uvs.at(strip[i]);
                    offU = i ? std::min(offU, u[0]) : u[0];
                    offV = i ? std::min(offV, u[1]) : u[1];
                }
                offU = std::floor(offU), offV = std::floor(offV);
                std::vector<V2> su;
                for (size_t i = 0; i < strip.size(); ++i)
                {
                    const V2 &u = uvs.at(strip[i]);
                    su.push_back({u[0] - offU, u[1] - offV});
                    maxU = i ? std::max(maxU, su.back()[0]) : su.back()[0];
                    maxV = i ? std::max(maxV, su.back()[1]) : su.back()[1];
                }
                const double uvScale[2] = {std::max(1.0, maxU / 2), std::max(1.0, maxV / 2)};
                std::vector<Raw> raw;
                for (size_t i = 0; i < strip.size(); ++i)
                {
                    const V3 &v = verts.at(strip[i]);
                    const V3 n = normals.empty() ? V3{0.0, 0.0, 1.0} : normals.at(strip[i]);
                    Raw r{};
                    for (int k = 0; k < 3; ++k)
                        r.p[k] = q16((v[k] - centre[k]) / scale[k]);
                    for (int k = 0; k < 2; ++k)
                        r.uv[k] = static_cast<uint8_t>(clampI(pyRound(su[i][k] / uvScale[k] * 127.5), 0, 255));
                    for (int k = 0; k < 3; ++k)
                        r.n[k] = static_cast<int8_t>(clampI(pyRound(n[k] * 127), -127, 127));
                    raw.push_back(r);
                }
                const Bytes packet = encode(raw);
                // the mesh's box in the geometry's int16 space, two units out (as the game's own)
                int box[6];
                for (int k = 0; k < 3; ++k)
                {
                    int lo = raw[0].p[k], hi = raw[0].p[k];
                    for (const Raw &r : raw)
                        lo = std::min<int>(lo, r.p[k]), hi = std::max<int>(hi, r.p[k]);
                    box[k] = std::max(-32768, lo - 2), box[3 + k] = std::min(32767, hi + 2);
                }
                for (int k = 0; k < 3; ++k)
                {
                    gbox[k] = firstMesh ? box[k] : std::min(gbox[k], box[k]);
                    gbox[3 + k] = firstMesh ? box[3 + k] : std::max(gbox[3 + k], box[3 + k]);
                }
                firstMesh = false;
                V3 mc{};
                for (int k = 0; k < 3; ++k)
                {
                    double sum = 0.0;
                    for (const uint32_t i : strip)
                        sum += verts[i][k];
                    mc[k] = sum / static_cast<double>(strip.size());
                }
                double mr = 0.0;
                for (size_t i = 0; i < strip.size(); ++i)
                {
                    const V3 &v = verts[strip[i]];
                    const double r = std::pow(std::pow(v[0] - mc[0], 2) + std::pow(v[1] - mc[1], 2) + std::pow(v[2] - mc[2], 2), 0.5);
                    mr = i ? std::max(mr, r) : r;
                }
                for (const double x : {mc[0], mc[1], mc[2], mr, uvScale[0], uvScale[1]})
                    addf(meshes, x);
                add32(meshes, 0);
                add32(meshes, static_cast<uint32_t>(dma.size()));
                add16(meshes, static_cast<uint32_t>(strip.size() - 2));
                add16(meshes, static_cast<uint32_t>(static_cast<int16_t>(material)));
                for (int k = 0; k < 6; ++k)
                    add16(meshes, static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(box[k]))));
                dma.insert(dma.end(), packet.begin(), packet.end());
                total += static_cast<uint32_t>(strip.size());
            }
            const auto n = static_cast<uint32_t>(groups.size());
            Bytes body;
            for (const double x : {centre[0], centre[1], centre[2], radius})
                addf(body, x);
            add32(body, (n << 20) | (static_cast<uint32_t>(dma.size()) & 0xFFFFF));
            add32(body, 7);
            add16(body, total & 0xFFFF);
            add16(body, 0x40 + n * 0x30);
            for (int k = 0; k < 6; ++k)
                add16(body, static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(gbox[k]))));
            for (const double x : {scale[0], scale[1], scale[2], centre[0], centre[1], centre[2]})
                addf(body, x);
            body.insert(body.end(), meshes.begin(), meshes.end());
            body.insert(body.end(), dma.begin(), dma.end());
            padTo(body, 4);
            const auto listAt = static_cast<uint32_t>(32 + body.size());
            if (uint64_t(templateGeom) + 32 > m.d.size())
                throw Bad("a geometry past the template's end");
            Bytes block(m.d.begin() + templateGeom, m.d.begin() + templateGeom + 32);
            put32(block, 0x0C, listAt);
            put32(block, 0x10, static_cast<uint32_t>(materials.size()));
            put32(block, 0x14, static_cast<uint32_t>(materials.size()));
            block.insert(block.end(), body.begin(), body.end());
            for (const uint32_t mat : materials)
                add32(block, mat);
            padTo(block, 16);
            inner = {0x0C};
            for (size_t i = 0; i < materials.size(); ++i)
                inner.push_back(listAt + static_cast<uint32_t>(4 * i));
            return block;
        }

        const std::map<std::string, std::string> &vcGeneric()
        {
            static const std::map<std::string, std::string> map = {{"vehiclegeneric256", "xv_vehiclegeneric_sl"},
                                                                     {"vehiclescratch64", "xv_scratch_sl"},
                                                                     {"vehiclegrunge256", "xv_vehiclegrunge_sl"},
                                                                     {"vehiclesteering", "xv_vehiclesteering_sl"}};
            return map;
        }

        // rwinfo.read_txd's names
        std::vector<std::string> txdNames(const Bytes &d)
        {
            const auto all = chunks(d, 0, d.size());
            if (all.empty() || all[0].kind != kTexDict)
                throw Bad("the .txd is not a texture dictionary");
            std::vector<std::string> out;
            for (const Chunk &c : chunks(d, all[0].at, all[0].end))
                if (c.kind == kTexNative)
                {
                    const uint64_t s = need(child(d, c.at, c.end, kStruct), "texture struct").first;
                    out.push_back(cString(d, s + 8, s + 40));
                }
            return out;
        }

        // dff2mdl.convert
        Bytes convertDff(const Bytes &dffData, const Bytes &templ, const Bytes *txd, std::string &report)
        {
            const Mdl m(templ);
            const Dff dff = readDff(dffData);
            const auto &frames = dff.frames;
            std::map<std::string, size_t> byName;
            for (size_t i = 0; i < frames.size(); ++i)
                byName[lower(frames[i].name)] = i;
            const uint32_t group = m.ptr(0x20);
            Bytes data(templ.begin(), templ.begin() + m.dataSize);
            std::set<uint32_t> relocs;
            for (const uint32_t r : m.relocs)
                if (r < m.dataSize)
                    relocs.insert(r);
            std::optional<std::set<std::string>> textures;
            std::map<std::string, std::string> textureNames;
            if (txd)
            {
                textures.emplace();
                for (const std::string &name : txdNames(*txd))
                {
                    textureNames[lower(name)] = name;
                    textures->insert(lower(name));
                }
            }

            const uint32_t root = m.ptr(group + 4);
            const uint32_t extraList = m.ptr(0x28), extraCount = u32(templ, 0x24);
            const std::vector<uint32_t> groupElements = m.elements(group);
            std::vector<uint32_t> elements = groupElements;
            if (extraList && extraCount <= 64)
                for (uint32_t k = 0; k < extraCount; ++k)
                {
                    const uint32_t e = m.ptr(extraList + 4 * k);
                    if (e && std::find(groupElements.begin(), groupElements.end(), e) == groupElements.end())
                        elements.push_back(e);
                }

            // a VC frame's matrix relative to the clump's root (the root's own is the vehicle's place: the game sets it)
            auto rrVc = [&](size_t i) {
                Mat mat = kIdentity;
                for (int guard = 0; frames.at(i).parent >= 0 && guard < 64; ++guard)
                {
                    Mat local{};
                    std::copy(frames[i].rot, frames[i].rot + 9, local.r);
                    std::copy(frames[i].pos, frames[i].pos + 3, local.p);
                    mat = matMul(mat, local);
                    i = static_cast<size_t>(frames[i].parent);
                }
                return mat;
            };
            // a VCS node's, from the data as it stands (extras' nodes hang free: their own)
            auto rrVcs = [&](uint32_t n) {
                Mat mat = kIdentity;
                for (int guard = 0; n && n != root && guard < 64; ++guard)
                {
                    Mat local{};
                    for (int k = 0; k < 9; ++k)
                        local.r[k] = f32(data, n + 0x10 + (k / 3) * 16 + (k % 3) * 4);
                    for (int k = 0; k < 3; ++k)
                        local.p[k] = f32(data, n + 0x40 + k * 4);
                    mat = matMul(mat, local);
                    n = m.ptr(n + 4);
                }
                return mat;
            };

            // the nodes: where the VC frame of the same name is, relative to the root - whatever the trees between
            std::vector<std::pair<uint32_t, std::string>> nodeList;
            m.nodes(root, nodeList);
            std::vector<uint32_t> placed;
            for (const auto &entry : nodeList)
                if (entry.first != root)
                    placed.push_back(entry.first);
            for (const uint32_t e : elements)
            {
                const uint32_t n = m.ptr(e + 4);
                if (n && std::find(placed.begin(), placed.end(), n) == placed.end())
                    placed.push_back(n);
            }
            int moved = 0;
            for (const uint32_t n : placed)
            {
                const std::string name = lower(m.cstr(m.ptr(n + 0xA8)));
                const auto it = byName.find(name);
                if (name.empty() || it == byName.end() || frames[it->second].parent < 0)
                    continue;
                const Mat want = rrVc(it->second);
                const uint32_t parent = m.ptr(n + 4);
                const Mat loc = parent && parent != root ? matMul(want, matInv(rrVcs(parent))) : want;
                for (int k = 0; k < 9; ++k)
                    putf(data, n + 0x10 + (k / 3) * 16 + (k % 3) * 4, loc.r[k]);
                for (int k = 0; k < 3; ++k)
                    putf(data, n + 0x40 + k * 4, loc.p[k]);
                ++moved;
            }

            // the paint lists: emptied; the new paint materials go in, with the template's paint effects
            constexpr uint32_t kLists[2][2] = {{0x2C, 30}, {0xA4, 25}};
            uint32_t paintFx = 0;
            for (uint32_t k = 0; k < kLists[0][1]; ++k)
            {
                const uint32_t mat = m.ptr(kLists[0][0] + 4 * k);
                if (!mat)
                    break;
                if (m.relocs.count(mat + 12))
                {
                    paintFx = u32(templ, mat + 12);
                    break;
                }
            }
            for (const auto &list : kLists)
                for (uint32_t k = 0; k < list[1]; ++k)
                {
                    put32(data, list[0] + 4 * k, 0);
                    relocs.erase(list[0] + 4 * k);
                }
            uint32_t paintUsed[2] = {0, 0};
            std::map<std::string, uint32_t> names;
            auto stringAt = [&](const std::string &text) {
                const auto it = names.find(text);
                if (it != names.end())
                    return it->second;
                const auto at = static_cast<uint32_t>(data.size());
                names[text] = at;
                data.insert(data.end(), text.begin(), text.begin() + static_cast<ptrdiff_t>(std::min<size_t>(text.size(), 31)));
                data.push_back(0);
                padTo(data, 4);
                return at;
            };
            auto material = [&](const Material &mat) {
                const bool primary = mat.rgba[0] == 0x3C && mat.rgba[1] == 0xFF && mat.rgba[2] == 0x00;
                const bool secondary = mat.rgba[0] == 0xFF && mat.rgba[1] == 0x00 && mat.rgba[2] == 0xAF;
                const int kind = primary ? 0 : secondary ? 1 : -1;
                std::string tex = lower(mat.texture);
                if (textures && !tex.empty() && !textures->count(tex))
                {
                    const auto g = vcGeneric().find(tex);
                    tex = g != vcGeneric().end() ? g->second : std::string();
                }
                uint32_t nameAt = 0;
                if (!tex.empty())
                {
                    const auto named = textureNames.find(tex);
                    nameAt = stringAt(named != textureNames.end() ? named->second : tex);
                }
                padTo(data, 4);
                const auto at = static_cast<uint32_t>(data.size());
                add32(data, nameAt);
                for (int k = 0; k < 4; ++k)
                    data.push_back(kind >= 0 ? 255 : mat.rgba[k]);
                add32(data, 2);
                add32(data, kind >= 0 ? paintFx : 0);
                if (nameAt)
                    relocs.insert(at);
                if (kind >= 0 && paintFx)
                    relocs.insert(at + 12);
                if (kind >= 0)
                {
                    if (paintUsed[kind] < kLists[kind][1])
                    {
                        put32(data, kLists[kind][0] + 4 * paintUsed[kind], at);
                        relocs.insert(kLists[kind][0] + 4 * paintUsed[kind]);
                        ++paintUsed[kind];
                    }
                    else
                        report += kind == 0 ? "more first-colour materials than the list holds; " : "more second-colour materials than the list holds; ";
                }
                return at;
            };

            // Each VCS element gets VC parts (dff2mdl.convert): the part of its node's name; X_dummy's two elements X_hi_ok
            // and X_hi_dam (X_hi_ok again when there is no damaged one); a helicopter's rotors their VC rotor. Left over:
            // damaged parts, extras and LODs go; the rest joins the chassis (or a boat's hull). Elements no part reaches
            // are emptied.
            std::vector<std::string> namesOf;
            for (const Atomic &at : dff.atomics)
                namesOf.push_back(lower(frames.at(at.frame).name));
            std::vector<char> used(namesOf.size(), 0);
            auto take = [&](const std::string &name, bool keep) -> int {
                for (size_t k = 0; k < namesOf.size(); ++k)
                    if (namesOf[k] == name && (keep || !used[k]))
                    {
                        used[k] = 1;
                        return static_cast<int>(k);
                    }
                return -1;
            };
            auto endsWith = [](const std::string &s, const std::string &tail) {
                return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
            };
            std::vector<std::pair<std::string, std::vector<uint32_t>>> byNode;
            for (const uint32_t e : elements)
            {
                const std::string name = lower(m.cstr(m.ptr(m.ptr(e + 4) + 0xA8)));
                auto it = std::find_if(byNode.begin(), byNode.end(), [&](const auto &p) { return p.first == name; });
                if (it == byNode.end())
                    byNode.push_back({name, {e}});
                else
                    it->second.push_back(e);
            }
            // still rotors from VC's; the moving ones (a blur disc the game fades in with the speed) stay VCS's
            static const std::map<std::string, std::string> kAliases = {{"static_rotor", "toprotor"}, {"static_rotor2", "rearrotor"}};
            auto keptNode = [](const std::string &name) { return name == "moving_rotor" || name == "moving_rotor2"; };
            std::map<uint32_t, std::vector<int>> assign;
            for (const auto &[name, els] : byNode)
            {
                if (endsWith(name, "_dummy"))
                {
                    // intact elements have object flag 4 (+2: shown), damaged ones not, in any order; a dummy can hold more
                    // than one of a kind (a van's door and its window): VC's part goes into the first, the others are emptied
                    std::vector<uint32_t> intact, damaged;
                    for (const uint32_t e : els)
                        ((u8(templ, e + 2) & 4) ? intact : damaged).push_back(e);
                    if (intact.empty())
                    {
                        intact.assign(els.begin(), els.begin() + 1);
                        damaged.assign(els.begin() + 1, els.end());
                    }
                    const std::string base = name.substr(0, name.size() - 6);
                    const int ok = take(base + "_hi_ok", false), dam = take(base + "_hi_dam", false);
                    if (!intact.empty() && (ok >= 0 || dam >= 0))
                        assign[intact[0]] = {ok >= 0 ? ok : dam};
                    if (!damaged.empty() && (ok >= 0 || dam >= 0))
                        assign[damaged[0]] = {dam >= 0 ? dam : ok};
                }
                else if (const auto alias = kAliases.find(name); alias != kAliases.end())
                {
                    const int k = take(alias->second, true);
                    if (k >= 0)
                        assign[els[0]] = {k};
                }
                else
                    for (const uint32_t e : els)
                    {
                        const int k = take(name, false);
                        if (k >= 0)
                            assign[e] = {k};
                    }
            }
            uint32_t host = 0;
            for (const auto &[name, els] : byNode)
                if (name == "chassis_hi" || name == "boat_hi")
                {
                    host = els[0];
                    break;
                }
            std::vector<std::string> dropped;
            for (size_t k = 0; k < namesOf.size(); ++k)
            {
                if (used[k])
                    continue;
                const std::string &n = namesOf[k];
                if (endsWith(n, "_dam") || n.rfind("extra", 0) == 0 || endsWith(n, "_vlo") || !host)
                    dropped.push_back(n);
                else
                    assign[host].push_back(static_cast<int>(k));
            }
            padTo(data, 16);
            std::vector<std::string> replaced, blanked;
            uint32_t anyGeom = 0;
            for (const uint32_t e : elements)
                if (!anyGeom)
                    anyGeom = m.ptr(e + 0x14);

            auto put = [&](uint32_t element, const std::vector<V3> &verts, const std::vector<V3> &normals, const std::vector<V2> &uvs,
                           const std::vector<std::pair<int, std::vector<uint32_t>>> &groups, const std::vector<uint32_t> &mats) {
                std::vector<uint32_t> inner;
                const uint32_t own = m.ptr(element + 0x14);
                Bytes block = buildGeometry(m, own ? own : anyGeom, verts, normals, uvs, groups, mats, inner);
                padTo(data, 16); // the DMA data inside on qwords
                const auto at = static_cast<uint32_t>(data.size());
                put32(block, 0x0C, at + u32(block, 0x0C));
                data.insert(data.end(), block.begin(), block.end());
                for (const uint32_t r : inner)
                    relocs.insert(at + r);
                put32(data, element + 0x14, at);
            };
            auto blank = [&](uint32_t element) {
                const uint32_t mat = material(Material{{0, 0, 0, 0}, ""});
                put(element, {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}, {}, {{0, 0}, {0, 0}, {0, 0}}, {{0, {0, 1, 2}}}, {mat});
                blanked.push_back(m.cstr(m.ptr(m.ptr(element + 4) + 0xA8)));
            };

            std::vector<std::string> kept;
            for (const uint32_t e : elements)
            {
                const uint32_t node = m.ptr(e + 4);
                const auto found = assign.find(e);
                if ((found == assign.end() || found->second.empty()) && keptNode(lower(m.cstr(m.ptr(node + 0xA8)))))
                {
                    kept.push_back(m.cstr(m.ptr(node + 0xA8)));
                    continue;
                }
                if (found == assign.end() || found->second.empty())
                {
                    blank(e);
                    continue;
                }
                const std::vector<int> &parts = found->second;
                const Mat toNode = matInv(rrVcs(node));
                std::vector<V3> verts, normals;
                std::vector<V2> uvs;
                std::vector<uint32_t> mats;
                std::vector<std::pair<std::pair<std::string, std::array<uint8_t, 4>>, std::pair<int, std::vector<std::array<uint32_t, 3>>>>> byKey;
                bool hasNormals = false;
                for (const int k : parts)
                    hasNormals = hasNormals || !dff.geometries.at(dff.atomics[k].geometry).normals.empty();
                std::string list;
                for (const int k : parts)
                {
                    const Geometry &g = dff.geometries.at(dff.atomics[k].geometry);
                    const Mat to = matMul(rrVc(dff.atomics[k].frame), toNode); // the part's frame -> the element's node
                    const auto base = static_cast<uint32_t>(verts.size());
                    for (const V3 &v : g.verts)
                        verts.push_back(matApply(to, v));
                    if (!g.normals.empty())
                        for (const V3 &n : g.normals)
                            normals.push_back(matApply(to, n, false));
                    else
                        normals.insert(normals.end(), g.verts.size(), V3{0.0, 0.0, 1.0});
                    uvs.insert(uvs.end(), g.uvs.begin(), g.uvs.end());
                    for (size_t mi = 0; mi < g.materials.size(); ++mi)
                    {
                        const Material &mat = g.materials[mi];
                        std::vector<std::array<uint32_t, 3>> tris;
                        for (const Tri &t : g.tris)
                            if (t.mat == mi)
                                tris.push_back({t.a + base, t.b + base, t.c + base});
                        if (tris.empty())
                            continue;
                        const auto key = std::make_pair(lower(mat.texture), std::array<uint8_t, 4>{mat.rgba[0], mat.rgba[1], mat.rgba[2], mat.rgba[3]});
                        auto it = std::find_if(byKey.begin(), byKey.end(), [&](const auto &p) { return p.first == key; });
                        if (it == byKey.end())
                        {
                            byKey.push_back({key, {static_cast<int>(mats.size()), {}}});
                            mats.push_back(material(mat));
                            it = byKey.end() - 1;
                        }
                        it->second.second.insert(it->second.second.end(), tris.begin(), tris.end());
                    }
                    list += (list.empty() ? "" : "+") + namesOf[static_cast<size_t>(k)];
                }
                std::vector<std::pair<int, std::vector<uint32_t>>> groups;
                for (const auto &entry : byKey)
                    for (const auto &tile : uvTiles(entry.second.second, uvs))
                    {
                        std::vector<uint32_t> strip = stripOf(tile);
                        if (!strip.empty())
                            groups.push_back({entry.second.first, std::move(strip)});
                    }
                if (groups.empty())
                {
                    blank(e);
                    continue;
                }
                put(e, verts, hasNormals ? normals : std::vector<V3>(), uvs, groups, mats);
                replaced.push_back(m.cstr(m.ptr(node + 0xA8)) + "<" + list);
            }

            // the file's tail moves as a whole: global table, relocation table, the strings after it
            const uint64_t G = m.globalTab, R = m.relocTab, T = R + 4ull * m.numRelocs, F = m.fileSize;
            if (T > F || F > templ.size())
                throw Bad("the template's tables do not fit the file");
            const uint64_t dataSize = data.size();
            const int64_t shiftG = int64_t(dataSize) - int64_t(G);
            for (const uint32_t r : m.relocs)
                if (r >= G && r < R)
                    relocs.insert(static_cast<uint32_t>(int64_t(r) + shiftG));
            const uint64_t tailAt = dataSize + (R - G) + 4ull * relocs.size();
            const int64_t shiftT = int64_t(tailAt) - int64_t(T);
            for (const uint32_t r : m.relocs) // the template's own pointers into the tail
                if (r < m.dataSize)
                {
                    const uint32_t v = u32(templ, r);
                    if (v >= T && v < F && v == u32(data, r))
                        put32(data, r, static_cast<uint32_t>(int64_t(v) + shiftT));
                }
            Bytes gtab(templ.begin() + static_cast<ptrdiff_t>(G), templ.begin() + static_cast<ptrdiff_t>(R));
            for (const uint32_t r : m.relocs)
                if (r >= G && r < R)
                {
                    const uint32_t v = u32(gtab, r - G);
                    if (v >= T)
                        put32(gtab, r - G, static_cast<uint32_t>(int64_t(v) + shiftT));
                }
            data.insert(data.end(), gtab.begin(), gtab.end());
            const auto relocTab = static_cast<uint32_t>(data.size());
            for (const uint32_t r : relocs)
                add32(data, r);
            if (data.size() != tailAt)
                throw Bad("the tail's place came out wrong");
            data.insert(data.end(), templ.begin() + static_cast<ptrdiff_t>(T), templ.begin() + static_cast<ptrdiff_t>(F));
            put32(data, 0, m.ident);
            put32(data, 4, m.shrink);
            put32(data, 8, static_cast<uint32_t>(data.size()));
            put32(data, 12, static_cast<uint32_t>(dataSize));
            put32(data, 16, relocTab);
            put32(data, 20, static_cast<uint32_t>(relocs.size()));
            put32(data, 24, static_cast<uint32_t>(dataSize));
            data[28] = static_cast<uint8_t>(m.numClasses), data[29] = static_cast<uint8_t>(m.numClasses >> 8);
            data[30] = static_cast<uint8_t>(m.numFuncs), data[31] = static_cast<uint8_t>(m.numFuncs >> 8);
            auto join = [](const std::vector<std::string> &v) {
                std::string out;
                for (const std::string &x : v)
                    out += (out.empty() ? "" : ", ") + x;
                return out.empty() ? std::string("none") : out;
            };
            char line[64];
            std::snprintf(line, sizeof(line), "%d nodes placed; elements: ", moved);
            report = line + join(replaced) + "; emptied: " + join(blanked) + "; kept: " + join(kept) + "; left out: " + join(dropped) +
                     (report.empty() ? "" : "; " + report);
            return data;
        }

        // ---- textures (txd2xtx) ----------------------------------------------------------------------------------------
        using RGBA = std::array<int, 4>;
        using Image = std::vector<std::vector<RGBA>>;

        std::array<int, 3> c565(uint32_t c)
        {
            const int r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
            return {r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2};
        }

        std::array<RGBA, 16> dxtColours(const Bytes &d, uint64_t at, bool four)
        {
            const uint32_t c0 = u16(d, at), c1 = u16(d, at + 2), bits = u32(d, at + 4);
            const auto a = c565(c0), b = c565(c1);
            RGBA cols[4];
            for (int k = 0; k < 3; ++k)
            {
                cols[0][k] = a[k], cols[1][k] = b[k];
                if (c0 > c1 || four)
                    cols[2][k] = (2 * a[k] + b[k]) / 3, cols[3][k] = (a[k] + 2 * b[k]) / 3;
                else
                    cols[2][k] = (a[k] + b[k]) / 2, cols[3][k] = 0;
            }
            cols[0][3] = cols[1][3] = cols[2][3] = 255;
            cols[3][3] = (c0 > c1 || four) ? 255 : 0;
            std::array<RGBA, 16> out;
            for (int i = 0; i < 16; ++i)
                out[i] = cols[bits >> (2 * i) & 3];
            return out;
        }

        Image decodeDxt(const Bytes &d, uint64_t base, int w, int h, int kind)
        {
            Image out(static_cast<size_t>(h), std::vector<RGBA>(static_cast<size_t>(w), RGBA{0, 0, 0, 0}));
            const uint64_t size = kind == 1 ? 8 : 16;
            uint64_t at = base;
            for (int by = 0; by < std::max(h, 4); by += 4)
                for (int bx = 0; bx < std::max(w, 4); bx += 4)
                {
                    std::array<RGBA, 16> px;
                    if (kind == 1)
                        px = dxtColours(d, at, false);
                    else
                    {
                        px = dxtColours(d, at + 8, true);
                        int al[16];
                        if (kind == 3)
                        {
                            uint64_t alpha = 0;
                            for (int i = 0; i < 8; ++i)
                                alpha |= uint64_t(u8(d, at + i)) << (8 * i);
                            for (int i = 0; i < 16; ++i)
                                al[i] = static_cast<int>(alpha >> (4 * i) & 15) * 17;
                        }
                        else
                        {
                            const int a0 = u8(d, at), a1 = u8(d, at + 1);
                            uint64_t bits = 0;
                            for (int i = 0; i < 6; ++i)
                                bits |= uint64_t(u8(d, at + 2 + i)) << (8 * i);
                            int table[8] = {a0, a1, 0, 0, 0, 0, 0, 0};
                            if (a0 > a1)
                                for (int k = 1; k <= 6; ++k)
                                    table[k + 1] = ((7 - k) * a0 + k * a1) / 7;
                            else
                            {
                                for (int k = 1; k <= 4; ++k)
                                    table[k + 1] = ((5 - k) * a0 + k * a1) / 5;
                                table[6] = 0, table[7] = 255;
                            }
                            for (int i = 0; i < 16; ++i)
                                al[i] = table[bits >> (3 * i) & 7];
                        }
                        for (int i = 0; i < 16; ++i)
                            px[i][3] = al[i];
                    }
                    for (int i = 0; i < 16; ++i)
                    {
                        const int x = bx + (i & 3), y = by + (i >> 2);
                        if (x < w && y < h)
                            out[y][x] = px[i];
                    }
                    at += size;
                }
            return out;
        }

        struct PcTexture
        {
            std::string name;
            Image rows;
        };

        std::vector<PcTexture> readPcTxd(const Bytes &d)
        {
            const auto all = chunks(d, 0, d.size());
            if (all.empty() || all[0].kind != kTexDict)
                throw Bad("the .txd is not a texture dictionary");
            std::vector<PcTexture> out;
            for (const Chunk &c : chunks(d, all[0].at, all[0].end))
            {
                if (c.kind != kTexNative)
                    continue;
                uint64_t at = need(child(d, c.at, c.end, kStruct), "texture struct").first;
                const uint32_t platform = u32(d, at);
                PcTexture t;
                t.name = cString(d, at + 8, at + 40);
                const uint32_t raster = u32(d, at + 72);
                const int w = u16(d, at + 80), h = u16(d, at + 82), compression = u8(d, at + 87);
                if (platform != 8)
                    throw Bad(t.name + ": not a D3D8 texture (VC PC's)");
                if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
                    throw Bad(t.name + ": a size out of range");
                at += 88;
                std::vector<RGBA> palette;
                if (raster & 0x2000)
                {
                    for (int i = 0; i < 256; ++i)
                        palette.push_back({u8(d, at + 4 * i), u8(d, at + 4 * i + 1), u8(d, at + 4 * i + 2), u8(d, at + 4 * i + 3)});
                    at += 1024;
                }
                else if (raster & 0x4000)
                {
                    for (int i = 0; i < 32; ++i)
                        palette.push_back({u8(d, at + 4 * i), u8(d, at + 4 * i + 1), u8(d, at + 4 * i + 2), u8(d, at + 4 * i + 3)});
                    at += 128;
                }
                const uint32_t size = u32(d, at);
                const uint64_t px = at + 4;
                if (px + size > d.size())
                    throw Bad(t.name + ": its pixels run past the end");
                const uint32_t fmt = raster & 0xF00;
                const auto W = static_cast<uint64_t>(w), H = static_cast<uint64_t>(h);
                t.rows.assign(H, std::vector<RGBA>(W));
                if (compression) // 1, 3; anything else read as 5 (as txd2xtx.decode_dxt does)
                    t.rows = decodeDxt(d, px, w, h, compression);
                else if (!palette.empty())
                {
                    std::vector<int> idx;
                    if (size >= W * H)
                        for (uint64_t i = 0; i < size; ++i)
                            idx.push_back(u8(d, px + i));
                    else
                        for (uint64_t i = 0; i < size; ++i)
                            idx.push_back(u8(d, px + i) & 15), idx.push_back(u8(d, px + i) >> 4);
                    for (uint64_t y = 0; y < H; ++y)
                        for (uint64_t x = 0; x < W; ++x)
                        {
                            const size_t i = static_cast<size_t>(y * W + x);
                            if (i >= idx.size() || static_cast<size_t>(idx[i]) >= palette.size())
                                throw Bad(t.name + ": a palette index out of range");
                            RGBA p = palette[static_cast<size_t>(idx[i])];
                            if (fmt == 0x600)
                                p[3] = 255;
                            t.rows[y][x] = p;
                        }
                }
                else if (fmt == 0x500 || fmt == 0x600)
                {
                    for (uint64_t y = 0; y < H; ++y)
                        for (uint64_t x = 0; x < W; ++x)
                        {
                            const uint64_t o = px + (y * W + x) * 4;
                            t.rows[y][x] = {u8(d, o + 2), u8(d, o + 1), u8(d, o), fmt == 0x500 ? u8(d, o + 3) : 255};
                        }
                }
                else if (fmt == 0x100 || fmt == 0x200 || fmt == 0x300 || fmt == 0xA00)
                {
                    for (uint64_t y = 0; y < H; ++y)
                        for (uint64_t x = 0; x < W; ++x)
                        {
                            const uint32_t v = u16(d, px + (y * W + x) * 2);
                            RGBA p;
                            if (fmt == 0x200)
                            {
                                const auto c = c565(v);
                                p = {c[0], c[1], c[2], 255};
                            }
                            else if (fmt == 0x300)
                                p = {int(v >> 8 & 15) * 17, int(v >> 4 & 15) * 17, int(v & 15) * 17, int(v >> 12 & 15) * 17};
                            else
                            {
                                const int r = v >> 10 & 31, g = v >> 5 & 31, b = v & 31;
                                p = {r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, ((v >> 15) || fmt == 0xA00) ? 255 : 0};
                            }
                            t.rows[y][x] = p;
                        }
                }
                else if (fmt == 0x400)
                {
                    for (uint64_t y = 0; y < H; ++y)
                        for (uint64_t x = 0; x < W; ++x)
                        {
                            const int l = u8(d, px + y * W + x);
                            t.rows[y][x] = {l, l, l, 255};
                        }
                }
                else
                    throw Bad(t.name + ": a raster format not handled");
                out.push_back(std::move(t));
            }
            return out;
        }

        Image halve(const Image &rows)
        {
            const size_t h = rows.size(), w = rows[0].size();
            if (w == 1 && h == 1)
                return rows;
            const size_t sx = w > 1 ? 2 : 1, sy = h > 1 ? 2 : 1;
            Image out;
            for (size_t y = 0; y < h; y += sy)
            {
                std::vector<RGBA> row;
                for (size_t x = 0; x < w; x += sx)
                {
                    const int n = static_cast<int>(sx * sy);
                    RGBA p{};
                    for (int k = 0; k < 4; ++k)
                    {
                        int sum = 0;
                        for (size_t j = 0; j < sy; ++j)
                            for (size_t i = 0; i < sx; ++i)
                                sum += rows.at(y + j).at(x + i)[k];
                        p[k] = (sum + n / 2) / n;
                    }
                    row.push_back(p);
                }
                out.push_back(std::move(row));
            }
            return out;
        }

        struct RgbaHash
        {
            size_t operator()(const RGBA &p) const { return size_t(p[0]) | size_t(p[1]) << 8 | size_t(p[2]) << 16 | size_t(p[3]) << 24; }
        };

        // txd2xtx.quantise: median cut on RGBA -> (palette, indices)
        void quantise(const Image &rows, size_t colours, std::vector<RGBA> &palette, std::vector<std::vector<int>> &indices)
        {
            std::unordered_map<RGBA, int64_t, RgbaHash> counts;
            std::vector<RGBA> order; // first appearance (Python's dict order)
            for (const auto &row : rows)
                for (const RGBA &p : row)
                    if (counts[p]++ == 0)
                        order.push_back(p);
            std::unordered_map<RGBA, int, RgbaHash> where;
            palette.clear();
            if (order.size() <= colours)
            {
                palette = order;
                std::sort(palette.begin(), palette.end());
                for (size_t i = 0; i < palette.size(); ++i)
                    where[palette[i]] = static_cast<int>(i);
            }
            else
            {
                std::vector<std::vector<RGBA>> boxes{order};
                while (boxes.size() < colours)
                {
                    int best = -1, bestRange = -1, bestK = 0;
                    for (size_t i = 0; i < boxes.size(); ++i)
                    {
                        if (boxes[i].size() < 2)
                            continue;
                        for (int k = 0; k < 4; ++k)
                        {
                            int lo = boxes[i][0][k], hi = lo;
                            for (const RGBA &p : boxes[i])
                                lo = std::min(lo, p[k]), hi = std::max(hi, p[k]);
                            if (hi - lo > bestRange)
                                best = static_cast<int>(i), bestRange = hi - lo, bestK = k;
                        }
                    }
                    if (best < 0 || bestRange <= 0)
                        break;
                    std::vector<RGBA> b = boxes[static_cast<size_t>(best)];
                    std::stable_sort(b.begin(), b.end(), [&](const RGBA &x, const RGBA &y) { return x[bestK] < y[bestK]; });
                    int64_t total = 0, run = 0;
                    for (const RGBA &p : b)
                        total += counts[p];
                    size_t cut = 1;
                    for (size_t j = 0; j < b.size(); ++j)
                    {
                        run += counts[b[j]];
                        if (run * 2 >= total)
                        {
                            cut = std::min(std::max<size_t>(j + 1, 1), b.size() - 1);
                            break;
                        }
                    }
                    boxes[static_cast<size_t>(best)] = std::vector<RGBA>(b.begin(), b.begin() + static_cast<ptrdiff_t>(cut));
                    boxes.insert(boxes.begin() + best + 1, std::vector<RGBA>(b.begin() + static_cast<ptrdiff_t>(cut), b.end()));
                }
                for (size_t i = 0; i < boxes.size(); ++i)
                {
                    int64_t n = 0;
                    for (const RGBA &p : boxes[i])
                        n += counts[p];
                    RGBA c{};
                    for (int k = 0; k < 4; ++k)
                    {
                        int64_t sum = 0;
                        for (const RGBA &p : boxes[i])
                            sum += int64_t(p[k]) * counts[p];
                        c[k] = static_cast<int>((sum + n / 2) / n);
                    }
                    palette.push_back(c);
                    for (const RGBA &p : boxes[i])
                        where[p] = static_cast<int>(i);
                }
            }
            indices.clear();
            for (const auto &row : rows)
            {
                std::vector<int> line;
                for (const RGBA &p : row)
                    line.push_back(where[p]);
                indices.push_back(std::move(line));
            }
        }

        int csm1(int i) { return (i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1); }

        int bitLength(size_t v)
        {
            int n = 0;
            while (v)
                ++n, v >>= 1;
            return n;
        }

        struct VcsTexture
        {
            std::string name;
            size_t width, height;
            int depth;
            std::vector<RGBA> palette;
            std::vector<std::vector<int>> indices;
            Bytes raw; // a VCS texture kept as it is: texels and palette (txd2xtx --keep)
        };

        // txd2xtx.read_xtx_raw: a VCS dictionary's textures as stored
        std::vector<VcsTexture> readXtxRaw(const Bytes &d)
        {
            if (u32(d, 0) != 0x00746578u)
                throw Bad("the kept dictionary is not a VCS texture dictionary");
            std::vector<VcsTexture> out;
            uint32_t link = u32(d, 0x28);
            for (int guard = 0; link && link != 0x28 && guard < 4096; ++guard)
            {
                const uint32_t tex = link - 8, ras = u32(d, tex), texels = u32(d, ras + 8), flags = u32(d, ras + 12);
                const int depth = static_cast<int>(flags >> 12 & 63);
                if (depth != 4 && depth != 8)
                    throw Bad("a kept texture of a depth not handled");
                VcsTexture t{cString(d, tex + 16, tex + 48), size_t(1) << (flags & 63), size_t(1) << (flags >> 6 & 63), depth, {}, {}, {}};
                const uint64_t size = t.width * t.height * static_cast<uint64_t>(depth) / 8 + (depth == 4 ? 16 : 256) * 4;
                if (texels + size > d.size())
                    throw Bad("a kept texture runs past the end");
                t.raw.assign(d.begin() + texels, d.begin() + static_cast<ptrdiff_t>(texels + size));
                out.push_back(std::move(t));
                link = u32(d, link);
            }
            return out;
        }

        // txd2xtx.write_xtx
        Bytes writeXtx(const std::vector<VcsTexture> &textures)
        {
            Bytes data(0x40, 0);
            std::vector<uint32_t> relocs;
            auto ptr = [&](uint32_t at, uint32_t value) {
                put32(data, at, value);
                relocs.push_back(at);
            };
            put32(data, 0x20, 6);
            std::vector<uint32_t> texelAt;
            for (const VcsTexture &t : textures)
            {
                padTo(data, 16);
                texelAt.push_back(static_cast<uint32_t>(data.size()));
                if (!t.raw.empty())
                {
                    data.insert(data.end(), t.raw.begin(), t.raw.end());
                    continue;
                }
                std::vector<RGBA> pal = t.palette;
                if (t.depth == 8)
                {
                    for (const auto &row : t.indices)
                        for (const int i : row)
                            data.push_back(static_cast<uint8_t>(i));
                    pal.resize(256, RGBA{0, 0, 0, 0});
                    std::vector<RGBA> stored(256);
                    for (int i = 0; i < 256; ++i)
                        stored[static_cast<size_t>(i)] = pal[static_cast<size_t>(csm1(i))];
                    pal = stored;
                }
                else
                {
                    for (const auto &row : t.indices)
                        for (size_t x = 0; x < t.width; x += 2)
                            data.push_back(static_cast<uint8_t>(row[x] | (x + 1 < row.size() ? row[x + 1] : 0) << 4));
                    pal.resize(16, RGBA{0, 0, 0, 0});
                }
                for (const RGBA &p : pal)
                {
                    data.push_back(static_cast<uint8_t>(p[0]));
                    data.push_back(static_cast<uint8_t>(p[1]));
                    data.push_back(static_cast<uint8_t>(p[2]));
                    data.push_back(static_cast<uint8_t>((p[3] * 128 + 127) / 255));
                }
            }
            padTo(data, 16);
            std::vector<uint32_t> texAt, links{0x28};
            for (size_t i = 0; i < textures.size(); ++i)
                texAt.push_back(static_cast<uint32_t>(data.size() + 0x60 * i));
            data.resize(data.size() + 0x60 * textures.size(), 0);
            for (const uint32_t a : texAt)
                links.push_back(a + 8);
            links.push_back(0x28);
            ptr(0x28, textures.empty() ? 0x28 : links[1]);
            ptr(0x2C, textures.empty() ? 0x28 : links[links.size() - 2]);
            for (size_t i = 0; i < textures.size(); ++i)
            {
                const VcsTexture &t = textures[i];
                const uint32_t at = texAt[i], ras = at + 0x50;
                ptr(at, ras);
                ptr(at + 4, 0x20);
                ptr(at + 8, links[i + 2]);
                ptr(at + 12, links[i]);
                for (size_t k = 0; k < 31 && k < t.name.size(); ++k)
                    data[at + 16 + k] = static_cast<uint8_t>(t.name[k]);
                const uint32_t flags = static_cast<uint32_t>(bitLength(t.width) - 1) | static_cast<uint32_t>(bitLength(t.height) - 1) << 6 |
                                       static_cast<uint32_t>(t.depth) << 12 | 1u << 20;
                put32(data, ras, 0);
                put32(data, ras + 4, (t.depth == 4 ? 0x450000u : 0x250000u) | static_cast<uint32_t>(t.width * t.depth / 8));
                ptr(ras + 8, texelAt[i]);
                put32(data, ras + 12, flags);
            }
            const auto dataSize = static_cast<uint32_t>(data.size());
            std::sort(relocs.begin(), relocs.end());
            for (const uint32_t r : relocs)
                add32(data, r);
            put32(data, 0, 0x00746578u);
            put32(data, 4, 0);
            put32(data, 8, static_cast<uint32_t>(data.size()));
            put32(data, 12, dataSize);
            put32(data, 16, dataSize);
            put32(data, 20, static_cast<uint32_t>(relocs.size()));
            put32(data, 24, 0);
            put32(data, 28, 0);
            return data;
        }

        bool readFile(const char *path, Bytes &out)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return true;
        }

        bool writeFile(const char *path, const Bytes &data)
        {
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
            return static_cast<bool>(out);
        }
    }

    std::string vcsName(const std::string &vcName)
    {
        static const std::map<std::string, std::string> kNames = {{"boxville", "boxvil"}, {"enforcer", "enforce"},
                                                                  {"fbiranch", "fbiran"}, {"police", "policem"},
                                                                  {"sabretur", "sabretb"}, {"washing", "washin"}};
        const std::string name = lower(vcName);
        const auto it = kNames.find(name);
        return it != kNames.end() ? it->second : name;
    }

    bool dffToMdl(const std::vector<uint8_t> &dff, const std::vector<uint8_t> &templateMdl, const std::vector<uint8_t> *txd,
                  std::vector<uint8_t> &out, std::string &report)
    {
        report.clear();
        try
        {
            out = convertDff(dff, templateMdl, txd, report);
            return true;
        }
        catch (const std::exception &e)
        {
            report = e.what();
            return false;
        }
    }

    bool txdToXtx(const std::vector<uint8_t> &txd, int maxSize, const std::vector<uint8_t> *keep, std::vector<uint8_t> &out,
                  std::string &report)
    {
        report.clear();
        try
        {
            std::vector<VcsTexture> textures;
            for (PcTexture &t : readPcTxd(txd))
            {
                Image rows = std::move(t.rows);
                while (rows[0].size() > static_cast<size_t>(maxSize) || rows.size() > static_cast<size_t>(maxSize))
                    rows = halve(rows);
                std::unordered_set<RGBA, RgbaHash> distinct;
                for (const auto &row : rows)
                    distinct.insert(row.begin(), row.end());
                VcsTexture v{t.name, rows[0].size(), rows.size(), distinct.size() <= 16 ? 4 : 8, {}, {}, {}};
                quantise(rows, size_t(1) << v.depth, v.palette, v.indices);
                char line[96];
                std::snprintf(line, sizeof(line), "%s%s %zux%zu %d bit", report.empty() ? "" : ", ", v.name.c_str(), v.width, v.height, v.depth);
                report += line;
                textures.push_back(std::move(v));
            }
            if (keep) // the VCS dictionary's own textures the .txd has no name for, after them
            {
                std::set<std::string> have;
                for (const VcsTexture &t : textures)
                    have.insert(lower(t.name));
                int kept = 0;
                for (VcsTexture &t : readXtxRaw(*keep))
                    if (!have.count(lower(t.name)))
                    {
                        textures.push_back(std::move(t));
                        ++kept;
                    }
                report += "; " + std::to_string(kept) + " of VCS's kept";
            }
            out = writeXtx(textures);
            return true;
        }
        catch (const std::exception &e)
        {
            report = e.what();
            return false;
        }
    }

    int commandLine(int argc, char *argv[])
    {
        if (argc < 2)
            return -1;
        const std::string command = argv[1];
        if (command != "--rwconv-dff" && command != "--rwconv-txd")
            return -1;
        Bytes a, b, c, out;
        std::string report;
        bool ok = false;
        if (command == "--rwconv-dff" && argc >= 5 && readFile(argv[2], a) && readFile(argv[3], b) && (argc < 6 || readFile(argv[5], c)))
            ok = dffToMdl(a, b, argc >= 6 ? &c : nullptr, out, report) && writeFile(argv[4], out);
        else if (command == "--rwconv-txd" && argc >= 4 && readFile(argv[2], a) && (argc < 5 || readFile(argv[4], b)))
            ok = txdToXtx(a, 128, argc >= 5 ? &b : nullptr, out, report) && writeFile(argv[3], out);
        else
            report = "usage: --rwconv-dff DFF TEMPLATE OUT [TXD] | --rwconv-txd TXD OUT [VCS.xtx]";
        std::printf("%s: %s (%zu bytes)\n", ok ? "ok" : "failed", report.c_str(), out.size());
        return ok ? 0 : 1;
    }
}
