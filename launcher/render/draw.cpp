#include "draw.h"

#include "core/png.h"

#include <algorithm>
#include <cstring>

namespace render
{
    DrawList draw;
    ImageAtlas imageAtlas;

    bool imagesInit(const std::filesystem::path &assetsDir)
    {
        constexpr int kAtlasW = 2048;
        constexpr int kAtlasH = 4096;
        imageAtlas.width = kAtlasW;
        imageAtlas.height = kAtlasH;
        imageAtlas.pixels.assign(static_cast<size_t>(kAtlasW) * static_cast<size_t>(kAtlasH) * 4, 0);

        struct Slot
        {
            ImageId id;
            const char *filename;
            int dstX, dstY, dstW, dstH;
        };
        const Slot slots[] = {
            {ImageId::Background, "fundo.png", 2, 2, 1920, 1326},
            {ImageId::GameLogo, "logo.png", 2, 1332, 730, 712},
            {ImageId::BrandLogo, "minha_logo.png", 738, 1332, 640, 280},
            {ImageId::GameIcon, "icone.png", 1384, 1332, 256, 256},
            {ImageId::SteamLogo, "steam.png", 1644, 1332, 256, 256},
            {ImageId::ControllerPs2, "controllers/ps2.png", 2, 2050, 640, 480},
            {ImageId::ControllerXbox, "controllers/xbox.png", 650, 2050, 640, 480},
            {ImageId::ControllerPs4, "controllers/ps4.png", 1300, 2050, 640, 480},
            {ImageId::ControllerPs5, "controllers/ps5.png", 2, 2540, 640, 480},
            {ImageId::ControllerSwitch, "controllers/switch.png", 650, 2540, 640, 480},
            {ImageId::ControllerKbMouse, "controllers/kb_mouse.png", 1300, 2540, 640, 480},
            {ImageId::MenuBackdrop, "menu_backdrop.png", 2, 3030, 1600, 900},
        };

        bool anyLoaded = false;
        for (const Slot &s : slots)
        {
            int srcW = 0, srcH = 0;
            std::vector<uint8_t> raw;
            if (!core::readPng(assetsDir / s.filename, srcW, srcH, raw))
                continue;
            std::vector<uint8_t> resized(static_cast<size_t>(s.dstW) * static_cast<size_t>(s.dstH) * 4);
            core::resizeRgba(raw.data(), srcW, srcH, resized.data(), s.dstW, s.dstH);

            // Premultiply alpha for clean GPU linear filtering & premultiplied blending
            for (size_t i = 0; i < resized.size(); i += 4)
            {
                const uint32_t a = resized[i + 3];
                resized[i + 0] = static_cast<uint8_t>((resized[i + 0] * a + 127) / 255);
                resized[i + 1] = static_cast<uint8_t>((resized[i + 1] * a + 127) / 255);
                resized[i + 2] = static_cast<uint8_t>((resized[i + 2] * a + 127) / 255);
            }

            // Copy into atlas with 1px clamped edge padding
            for (int y = -1; y <= s.dstH; ++y)
            {
                const int sy = std::clamp(y, 0, s.dstH - 1);
                const int ay = s.dstY + y;
                if (ay < 0 || ay >= kAtlasH)
                    continue;
                for (int x = -1; x <= s.dstW; ++x)
                {
                    const int sx = std::clamp(x, 0, s.dstW - 1);
                    const int ax = s.dstX + x;
                    if (ax < 0 || ax >= kAtlasW)
                        continue;
                    std::memcpy(&imageAtlas.pixels[(static_cast<size_t>(ay) * kAtlasW + ax) * 4],
                                &resized[(static_cast<size_t>(sy) * s.dstW + sx) * 4], 4);
                }
            }

            ImageEntry &e = imageAtlas.entries[static_cast<int>(s.id)];
            e.uv[0] = (static_cast<float>(s.dstX) + 0.5f) / static_cast<float>(kAtlasW);
            e.uv[1] = (static_cast<float>(s.dstY) + 0.5f) / static_cast<float>(kAtlasH);
            e.uv[2] = (static_cast<float>(s.dstX + s.dstW) - 0.5f) / static_cast<float>(kAtlasW);
            e.uv[3] = (static_cast<float>(s.dstY + s.dstH) - 0.5f) / static_cast<float>(kAtlasH);
            e.width = srcW;
            e.height = srcH;
            e.aspect = srcH > 0 ? static_cast<float>(srcW) / static_cast<float>(srcH) : 1.0f;
            e.valid = true;
            anyLoaded = true;
        }
        ++imageAtlas.generation;
        return anyLoaded;
    }

    const ImageEntry &imageInfo(ImageId id)
    {
        const int idx = static_cast<int>(id);
        if (idx >= 0 && idx < static_cast<int>(ImageId::Count))
            return imageAtlas.entries[idx];
        return imageAtlas.entries[0];
    }

    namespace
    {
        std::vector<Instance> &target() { return draw.layer == Layer::Overlay ? draw.overlay : draw.instances; }

        // a new instance with the current clip, offset and opacity applied; null when clipped away entirely
        Instance *add(Kind kind, Rect r, float grow)
        {
            r.x += draw.dx;
            r.y += draw.dy;
            if (r.right() + grow <= draw.clip[0] || r.bottom() + grow <= draw.clip[1] || r.x - grow >= draw.clip[2] ||
                r.y - grow >= draw.clip[3] || draw.opacity <= 0.0f)
                return nullptr;
            Instance &i = target().emplace_back();
            std::memset(&i, 0, sizeof(i));
            i.rect[0] = r.x;
            i.rect[1] = r.y;
            i.rect[2] = r.right();
            i.rect[3] = r.bottom();
            std::memcpy(i.clip, draw.clip, sizeof(i.clip));
            i.kind = kind;
            return &i;
        }

        Color faded(Color c) { return draw.opacity >= 1.0f ? c : core::fade(c, draw.opacity); }
    }

    void beginFrame(float width, float height)
    {
        draw.instances.clear();
        draw.overlay.clear();
        draw.layer = Layer::Base;
        draw.width = width;
        draw.height = height;
        draw.clip[0] = draw.clip[1] = 0;
        draw.clip[2] = width;
        draw.clip[3] = height;
        draw.opacity = 1;
        draw.dx = draw.dy = 0;
        draw.clipStack.clear();
        draw.opacityStack.clear();
        draw.offsetStack.clear();
    }

    void endFrame()
    {
        draw.instances.insert(draw.instances.end(), draw.overlay.begin(), draw.overlay.end());
        draw.overlay.clear();
    }

    Layer setLayer(Layer layer)
    {
        const Layer previous = draw.layer;
        draw.layer = layer;
        return previous;
    }

    void pushClip(Rect r)
    {
        draw.clipStack.push_back({draw.clip[0], draw.clip[1], draw.clip[2], draw.clip[3]});
        r.x += draw.dx;
        r.y += draw.dy;
        draw.clip[0] = std::max(draw.clip[0], r.x);
        draw.clip[1] = std::max(draw.clip[1], r.y);
        draw.clip[2] = std::min(draw.clip[2], r.right());
        draw.clip[3] = std::min(draw.clip[3], r.bottom());
    }

    void pushClipAbsolute(Rect r)
    {
        draw.clipStack.push_back({draw.clip[0], draw.clip[1], draw.clip[2], draw.clip[3]});
        draw.clip[0] = r.x;
        draw.clip[1] = r.y;
        draw.clip[2] = r.right();
        draw.clip[3] = r.bottom();
    }

    void popClip()
    {
        if (draw.clipStack.empty())
            return;
        const auto c = draw.clipStack.back();
        draw.clipStack.pop_back();
        std::memcpy(draw.clip, c.data(), sizeof(draw.clip));
    }

    Rect currentClip()
    {
        return {draw.clip[0] - draw.dx, draw.clip[1] - draw.dy, std::max(draw.clip[2] - draw.clip[0], 0.0f),
                std::max(draw.clip[3] - draw.clip[1], 0.0f)};
    }

    void pushOpacity(float opacity)
    {
        draw.opacityStack.push_back(draw.opacity);
        draw.opacity *= core::clamp01(opacity);
    }

    void popOpacity()
    {
        if (draw.opacityStack.empty())
            return;
        draw.opacity = draw.opacityStack.back();
        draw.opacityStack.pop_back();
    }

    void pushOffset(float dx, float dy)
    {
        draw.offsetStack.emplace_back(draw.dx, draw.dy);
        draw.dx += dx;
        draw.dy += dy;
    }

    void popOffset()
    {
        if (draw.offsetStack.empty())
            return;
        draw.dx = draw.offsetStack.back().first;
        draw.dy = draw.offsetStack.back().second;
        draw.offsetStack.pop_back();
    }

    void rectEx(Rect r, Color from, Color to, Corners radius, Color border, float borderWidth, Gradient direction)
    {
        if (r.w <= 0 || r.h <= 0)
            return;
        Instance *i = add(kRect, r, 1);
        if (!i)
            return;
        i->radii[0] = radius.tl;
        i->radii[1] = radius.tr;
        i->radii[2] = radius.br;
        i->radii[3] = radius.bl;
        i->color0 = faded(from);
        i->color1 = faded(to);
        i->border = faded(border);
        i->params[0] = borderWidth;
        i->params[3] = direction == Gradient::Horizontal ? 1.0f : 0.0f;
    }

    void rect(Rect r, Color fill, Corners radius) { rectEx(r, fill, fill, radius, 0, 0); }

    void gradient(Rect r, Color from, Color to, Corners radius, Gradient direction)
    {
        rectEx(r, from, to, radius, 0, 0, direction);
    }

    void outline(Rect r, Color border, float width, Corners radius) { rectEx(r, 0, 0, radius, border, width); }

    void shadow(Rect r, Color color, float sigma, float radius)
    {
        Instance *i = add(kShadow, r, 3 * sigma + 1);
        if (!i)
            return;
        i->radii[0] = i->radii[1] = i->radii[2] = i->radii[3] = radius;
        i->color0 = faded(color);
        i->params[1] = sigma;
    }

    void line(float x0, float y0, float x1, float y1, float thickness, Color color)
    {
        const float pad = thickness * 0.5f + 1;
        const Rect bounds{std::min(x0, x1) - pad, std::min(y0, y1) - pad, std::fabs(x1 - x0) + 2 * pad,
                          std::fabs(y1 - y0) + 2 * pad};
        Instance *i = add(kLine, bounds, 0);
        if (!i)
            return;
        i->uv[0] = x0 + draw.dx;
        i->uv[1] = y0 + draw.dy;
        i->uv[2] = x1 + draw.dx;
        i->uv[3] = y1 + draw.dy;
        i->color0 = faded(color);
        i->params[2] = thickness;
    }

    void arc(float cx, float cy, float radius, float thickness, float start, float sweep, Color color)
    {
        if (sweep <= 0)
            return;
        const float outer = radius + thickness * 0.5f + 1;
        Instance *i = add(kArc, {cx - outer, cy - outer, outer * 2, outer * 2}, 0);
        if (!i)
            return;
        i->uv[0] = start;
        i->uv[1] = std::min(sweep, 2 * core::kPi);
        i->uv[2] = radius;
        i->uv[3] = thickness;
        i->color0 = faded(color);
    }

    void quad(const float corners[8], Color color)
    {
        float x0 = corners[0], y0 = corners[1], x1 = corners[0], y1 = corners[1];
        for (int k = 1; k < 4; ++k)
        {
            x0 = std::min(x0, corners[k * 2]);
            x1 = std::max(x1, corners[k * 2]);
            y0 = std::min(y0, corners[k * 2 + 1]);
            y1 = std::max(y1, corners[k * 2 + 1]);
        }
        Instance *i = add(kQuad, {x0, y0, x1 - x0, y1 - y0}, 1);
        if (!i)
            return;
        float *slots[4] = {&i->uv[0], &i->uv[2], &i->radii[0], &i->radii[2]};
        for (int k = 0; k < 4; ++k)
        {
            slots[k][0] = corners[k * 2] + draw.dx;
            slots[k][1] = corners[k * 2 + 1] + draw.dy;
        }
        i->color0 = faded(color);
    }

    void circle(float cx, float cy, float radius, Color color)
    {
        rect({cx - radius, cy - radius, radius * 2, radius * 2}, color, radius);
    }

    void glyph(Rect r, const float uv[4], Color color, float blur)
    {
        Instance *i = add(kGlyph, r, blur > 0 ? blur + 1 : 0);
        if (!i)
            return;
        std::memcpy(i->uv, uv, sizeof(i->uv));
        i->color0 = faded(color);
        i->params[1] = std::max(blur, 0.0f);
    }

    void image(Rect r, ImageId id, Corners radius, Color tint, Color border, float borderWidth)
    {
        if (r.w <= 0 || r.h <= 0)
            return;
        const ImageEntry &e = imageInfo(id);
        if (!e.valid)
            return;
        Instance *i = add(kImage, r, 1);
        if (!i)
            return;
        std::memcpy(i->uv, e.uv, sizeof(i->uv));
        i->radii[0] = radius.tl;
        i->radii[1] = radius.tr;
        i->radii[2] = radius.br;
        i->radii[3] = radius.bl;
        i->color0 = faded(tint);
        i->border = faded(border);
        i->params[0] = borderWidth;
    }

    void imageCover(Rect r, ImageId id, Corners radius, Color tint)
    {
        if (r.w <= 0 || r.h <= 0)
            return;
        const ImageEntry &e = imageInfo(id);
        if (!e.valid)
            return;
        Instance *i = add(kImage, r, 1);
        if (!i)
            return;
        float u0 = e.uv[0], v0 = e.uv[1], u1 = e.uv[2], v1 = e.uv[3];
        const float boxAspect = r.w / r.h;
        if (boxAspect > e.aspect)
        {
            // Box is wider than image: crop top/bottom slightly
            const float keep = e.aspect / boxAspect;
            const float span = v1 - v0;
            const float trim = span * (1.0f - keep) * 0.4f; // bias slightly towards top of image
            v0 += trim;
            v1 -= span * (1.0f - keep) - trim;
        }
        else if (boxAspect < e.aspect)
        {
            // Box is taller than image: crop left/right
            const float keep = boxAspect / e.aspect;
            const float span = u1 - u0;
            const float trim = span * (1.0f - keep) * 0.5f;
            u0 += trim;
            u1 -= trim;
        }
        i->uv[0] = u0;
        i->uv[1] = v0;
        i->uv[2] = u1;
        i->uv[3] = v1;
        i->radii[0] = radius.tl;
        i->radii[1] = radius.tr;
        i->radii[2] = radius.br;
        i->radii[3] = radius.bl;
        i->color0 = faded(tint);
    }
}
