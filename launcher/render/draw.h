// The draw list: what the UI produces each frame and a renderer backend consumes.
//
// Every primitive is one fixed-size instance that the GPU expands to a quad and shades with a distance function, so
// the whole window (panels, borders, gradients, shadows, text, lines, arcs) is one buffer upload and one draw call.
// Submission order is painting order within a layer; the overlay layer (dialogs, sheets, tooltips, toasts) paints
// after the base layer whenever it was submitted.
//
// Coordinates are physical pixels, origin top-left. Clip, opacity and offset stacks apply to everything drawn
// inside them (scrolling, fading and sliding whole groups).
#pragma once

#include "core/math.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

namespace render
{
    using core::Color;
    using core::Rect;

    enum Kind : uint32_t
    {
        kRect = 0,   // rounded rectangle: gradient fill, optional border
        kGlyph = 1,  // a glyph from the font atlas
        kShadow = 2, // Gaussian-blurred rounded rectangle (shadows, glows)
        kLine = 3,   // antialiased segment with round caps
        kArc = 4,    // antialiased arc with round caps (spinners, rings)
        kQuad = 5,   // a filled convex quadrilateral, antialiased (corners in uv and radii)
        kImage = 6,  // RGBA8 texture from image_atlas with SDF rounded corners and optional border
    };

    enum class ImageId : int
    {
        None = 0,
        Background = 1, // fundo.png
        GameLogo = 2,   // logo.png
        BrandLogo = 3,  // minha_logo.png
        GameIcon = 4,   // icone.png
        SteamLogo = 5,  // steam.png
        ControllerPs2 = 6,    // controllers/ps2.png
        ControllerXbox = 7,   // controllers/xbox.png
        ControllerPs4 = 8,    // controllers/ps4.png
        ControllerPs5 = 9,    // controllers/ps5.png
        ControllerSwitch = 10,// controllers/switch.png
        ControllerKbMouse = 11,// controllers/kb_mouse.png
        Count = 12
    };

    struct ImageEntry
    {
        float uv[4] = {}; // normalized [0, 1] u0, v0, u1, v1 in imageAtlas
        int width = 0, height = 0;
        float aspect = 1.0f;
        bool valid = false;
    };

    struct ImageAtlas
    {
        int width = 0, height = 0;
        std::vector<uint8_t> pixels; // RGBA8 premultiplied
        uint32_t generation = 0;
        ImageEntry entries[static_cast<int>(ImageId::Count)];
    };
    extern ImageAtlas imageAtlas;

    bool imagesInit(const std::filesystem::path &assetsDir);
    const ImageEntry &imageInfo(ImageId id);

    // Keep in sync with Instance in render/d3d11/ui.hlsl.
    struct Instance
    {
        float rect[4];   // x0 y0 x1 y1
        float uv[4];     // glyph: atlas texel rect; line: endpoints; arc: start angle, sweep, radius, thickness
        float radii[4];  // corner radii: tl, tr, br, bl
        float clip[4];   // x0 y0 x1 y1
        Color color0;    // fill / gradient start / glyph / shadow / line colour
        Color color1;    // gradient end
        Color border;
        uint32_t kind;
        float params[4]; // x = border width, y = shadow sigma / glyph blur radius, z = line thickness, w = gradient (0 vertical, 1 horizontal)
    };
    static_assert(sizeof(Instance) == 96, "Instance is 96 bytes");

    enum class Layer
    {
        Base,
        Overlay
    };

    struct DrawList
    {
        std::vector<Instance> instances, overlay;
        Layer layer = Layer::Base;
        float width = 0, height = 0;
        float clip[4] = {};
        float opacity = 1;
        float dx = 0, dy = 0; // the current offset
        std::vector<std::array<float, 4>> clipStack;
        std::vector<float> opacityStack;
        std::vector<std::pair<float, float>> offsetStack;
    };

    extern DrawList draw;

    void beginFrame(float width, float height);
    void endFrame(); // the overlay appended to the base layer

    Layer setLayer(Layer layer); // returns the previous one

    void pushClip(Rect r);         // intersected with the current clip
    void pushClipAbsolute(Rect r); // replaces it (an overlay escaping the panel it was opened from)
    void popClip();
    Rect currentClip();

    void pushOpacity(float opacity);
    void popOpacity();

    void pushOffset(float dx, float dy); // added to the current offset (sliding a group)
    void popOffset();

    enum class Gradient
    {
        Vertical,
        Horizontal
    };

    struct Corners
    {
        float tl = 0, tr = 0, br = 0, bl = 0;
        Corners() = default;
        Corners(float all) : tl(all), tr(all), br(all), bl(all) {}
        Corners(float tl_, float tr_, float br_, float bl_) : tl(tl_), tr(tr_), br(br_), bl(bl_) {}
    };

    void rect(Rect r, Color fill, Corners radius = {});
    void gradient(Rect r, Color from, Color to, Corners radius = {}, Gradient direction = Gradient::Vertical);
    void rectEx(Rect r, Color from, Color to, Corners radius, Color border, float borderWidth,
                Gradient direction = Gradient::Vertical);
    void outline(Rect r, Color border, float width, Corners radius = {}); // border only
    void shadow(Rect r, Color color, float sigma, float radius = 0);
    void line(float x0, float y0, float x1, float y1, float thickness, Color color);
    // angles in radians, 0 = right, clockwise (y points down)
    void arc(float cx, float cy, float radius, float thickness, float start, float sweep, Color color);
    void circle(float cx, float cy, float radius, Color color);
    // a convex quadrilateral, corners in order around it (x0 y0 x1 y1 x2 y2 x3 y3)
    void quad(const float corners[8], Color color);
    void glyph(Rect r, const float uv[4], Color color, float blur = 0); // blur: a radius in pixels
    void image(Rect r, ImageId id, Corners radius = {}, Color tint = 0xFFFFFFFFu, Color border = 0, float borderWidth = 0);
    void imageCover(Rect r, ImageId id, Corners radius = {}, Color tint = 0xFFFFFFFFu);
}
