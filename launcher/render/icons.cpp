#include "icons.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <vector>

namespace render::icons
{
    namespace
    {
        constexpr float kPi = 3.14159265358979f;
        constexpr float kDesignBox = 20; // grid units drawn at `px` (the grid is 24: 2 units of air around the box)
        constexpr float kStroke = 1.5f;  // grid units

        struct V
        {
            float x, y;
        };

        float length(float x, float y) { return std::sqrt(x * x + y * y); }

        float segmentDistance(V p, V a, V b)
        {
            const float px = p.x - a.x, py = p.y - a.y, bx = b.x - a.x, by = b.y - a.y;
            const float bb = bx * bx + by * by;
            const float h = bb > 0 ? std::clamp((px * bx + py * by) / bb, 0.0f, 1.0f) : 0.0f;
            return length(px - bx * h, py - by * h);
        }

        float boxDistance(V p, V lo, V hi, float r)
        {
            const float cx = (lo.x + hi.x) * 0.5f, cy = (lo.y + hi.y) * 0.5f;
            const float hx = (hi.x - lo.x) * 0.5f, hy = (hi.y - lo.y) * 0.5f;
            r = std::min({r, hx, hy});
            const float qx = std::fabs(p.x - cx) - hx + r, qy = std::fabs(p.y - cy) - hy + r;
            return length(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
        }

        // A shape in pixel space and its signed distance (negative inside).
        struct Shape
        {
            enum Kind
            {
                Segment, // a, b, width: a stroke with round caps
                Disc,    // a, radius
                Ring,    // a, radius, width
                Arc,     // a, radius, width, start, sweep (radians, clockwise, sweep > 0): round caps
                Box,     // a, b (corners), radius: filled
                BoxRing, // a, b, radius, width: the outline, centred on the edge
                Stadium, // a, b, radius, width: the outline of a capsule around the segment
                Polygon, // points: a filled convex polygon
            } kind;
            explicit Shape(Kind k) : kind(k) {}
            bool cut = false; // removes coverage instead of adding it
            V a{}, b{};
            float radius = 0, width = 0, start = 0, sweep = 0;
            std::vector<V> points;

            float distance(V p) const
            {
                switch (kind)
                {
                case Segment: return segmentDistance(p, a, b) - width * 0.5f;
                case Disc: return length(p.x - a.x, p.y - a.y) - radius;
                case Ring: return std::fabs(length(p.x - a.x, p.y - a.y) - radius) - width * 0.5f;
                case Arc:
                {
                    float t = std::atan2(p.y - a.y, p.x - a.x) - start;
                    t -= std::floor(t / (2 * kPi)) * 2 * kPi;
                    if (t <= sweep)
                        return std::fabs(length(p.x - a.x, p.y - a.y) - radius) - width * 0.5f;
                    const float end = start + sweep;
                    const V e0{a.x + radius * std::cos(start), a.y + radius * std::sin(start)};
                    const V e1{a.x + radius * std::cos(end), a.y + radius * std::sin(end)};
                    return std::min(length(p.x - e0.x, p.y - e0.y), length(p.x - e1.x, p.y - e1.y)) - width * 0.5f;
                }
                case Box: return boxDistance(p, a, b, radius);
                case BoxRing: return std::fabs(boxDistance(p, a, b, radius)) - width * 0.5f;
                case Stadium: return std::fabs(segmentDistance(p, a, b) - radius) - width * 0.5f;
                case Polygon:
                {
                    // the largest distance outside an edge (exact inside and along edges; corners stay sharp)
                    float area = 0;
                    for (size_t i = 0; i < points.size(); ++i)
                    {
                        const V &u = points[i], &w = points[(i + 1) % points.size()];
                        area += u.x * w.y - w.x * u.y;
                    }
                    const float sign = area > 0 ? 1.0f : -1.0f;
                    float d = -1e9f;
                    for (size_t i = 0; i < points.size(); ++i)
                    {
                        const V &u = points[i], &w = points[(i + 1) % points.size()];
                        const float ex = w.x - u.x, ey = w.y - u.y, len = std::max(length(ex, ey), 1e-6f);
                        // outward normal of a clockwise (y down: positive area) polygon
                        const float nx = ey / len * sign, ny = -ex / len * sign;
                        d = std::max(d, (p.x - u.x) * nx + (p.y - u.y) * ny);
                    }
                    return d;
                }
                }
                return 1e9f;
            }
        };

        // Grid units in, pixels out. Small sizes snap coordinates so 1-pixel strokes sit on pixel centres (a minimise
        // line is one sharp row, not two grey ones).
        class Canvas
        {
        public:
            explicit Canvas(int px)
            {
                k = float(px) / kDesignBox;
                size = int(std::ceil(float(px) * 24.0f / kDesignBox)) + 2;
                center = float(size) * 0.5f;
                stroke = kStroke * k;
                snap = px <= 24;
                if (snap)
                    stroke = std::max(1.0f, std::round(stroke));
                oddStroke = int(std::lround(stroke)) % 2 == 1;
            }

            // a stroke `scale` times the usual weight
            void weight(float scale) { width = stroke * scale; }

            // only an axis-aligned stroke snaps: a diagonal one would be bent by it
            void line(float x0, float y0, float x1, float y1, bool cut = false)
            {
                Shape s(Shape::Segment);
                s.cut = cut;
                const bool vertical = std::fabs(x0 - x1) < 1e-3f, horizontal = std::fabs(y0 - y1) < 1e-3f;
                const bool axis = vertical || horizontal; // its ends snap too, to meet the strokes they join
                s.a = axis ? P(x0, y0) : V{raw(x0), raw(y0)};
                s.b = axis ? P(x1, y1) : V{raw(x1), raw(y1)};
                s.width = w();
                shapes.push_back(s);
            }

            void polyline(std::initializer_list<V> points, bool closed = false)
            {
                const V *p = points.begin();
                for (size_t i = 0; i + 1 < points.size(); ++i)
                    line(p[i].x, p[i].y, p[i + 1].x, p[i + 1].y);
                if (closed && points.size() > 2)
                    line(p[points.size() - 1].x, p[points.size() - 1].y, p[0].x, p[0].y);
            }

            void circle(float cx, float cy, float r)
            {
                Shape s(Shape::Ring);
                s.a = P(cx, cy);
                s.radius = r * k;
                s.width = w();
                shapes.push_back(s);
            }

            void disc(float cx, float cy, float r, bool cut = false)
            {
                Shape s(Shape::Disc);
                s.cut = cut;
                s.a = P(cx, cy);
                s.radius = std::max(r * k, cut ? 0.0f : 0.8f);
                shapes.push_back(s);
            }

            void dot(float cx, float cy) { disc(cx, cy, 1.15f); }

            // degrees, 0 = right, clockwise (y down); a negative sweep runs the other way
            void arc(float cx, float cy, float r, float startDeg, float sweepDeg)
            {
                Shape s(Shape::Arc);
                s.a = P(cx, cy);
                s.radius = r * k;
                s.width = w();
                float start = startDeg * kPi / 180, sweep = sweepDeg * kPi / 180;
                if (sweep < 0)
                {
                    start += sweep;
                    sweep = -sweep;
                }
                s.start = start;
                s.sweep = sweep;
                shapes.push_back(s);
            }

            // an arc with an arrowhead at its end, pointing the way it runs
            void arcArrow(float cx, float cy, float r, float startDeg, float sweepDeg, float head)
            {
                arc(cx, cy, r, startDeg, sweepDeg);
                const float end = (startDeg + sweepDeg) * kPi / 180;
                const float dir = sweepDeg >= 0 ? 1.0f : -1.0f;
                arrowhead(cx + r * std::cos(end), cy + r * std::sin(end), -std::sin(end) * dir, std::cos(end) * dir, head);
            }

            // two strokes back from the tip, 45 degrees either side of the direction
            void arrowhead(float tx, float ty, float dx, float dy, float len)
            {
                const float n = std::max(length(dx, dy), 1e-6f);
                dx /= n;
                dy /= n;
                const float c = std::cos(kPi / 4), s = std::sin(kPi / 4);
                line(tx, ty, tx - len * (dx * c - dy * s), ty - len * (dx * s + dy * c));
                line(tx, ty, tx - len * (dx * c + dy * s), ty - len * (-dx * s + dy * c));
            }

            void box(float x0, float y0, float x1, float y1, float r)
            {
                Shape s(Shape::BoxRing);
                s.a = P(x0, y0);
                s.b = P(x1, y1);
                s.radius = r * k;
                s.width = w();
                shapes.push_back(s);
            }

            void fillBox(float x0, float y0, float x1, float y1, float r, bool cut = false)
            {
                Shape s(Shape::Box);
                s.cut = cut;
                s.a = P(x0, y0);
                s.b = P(x1, y1);
                s.radius = r * k;
                shapes.push_back(s);
            }

            void stadium(float x0, float y0, float x1, float y1, float r)
            {
                Shape s(Shape::Stadium);
                s.a = P(x0, y0);
                s.b = P(x1, y1);
                s.radius = r * k;
                s.width = w();
                shapes.push_back(s);
            }

            void polygon(std::initializer_list<V> points)
            {
                Shape s(Shape::Polygon);
                for (const V &p : points)
                    s.points.push_back(P(p.x, p.y));
                shapes.push_back(s);
            }

            void render(raster::Bitmap &out) const
            {
                out.width = out.height = size;
                out.left = out.top = -size / 2;
                out.coverage.assign(size_t(size) * size_t(size), 0);
                for (int y = 0; y < size; ++y)
                    for (int x = 0; x < size; ++x)
                    {
                        const V p{float(x) + 0.5f, float(y) + 0.5f};
                        float c = 0;
                        for (const Shape &s : shapes)
                        {
                            const float cover = std::clamp(0.5f - s.distance(p), 0.0f, 1.0f);
                            c = s.cut ? std::min(c, 1.0f - cover) : std::max(c, cover);
                        }
                        out.coverage[size_t(y) * size_t(size) + size_t(x)] = uint8_t(c * 255.0f + 0.5f);
                    }
            }

        private:
            float raw(float g) const { return center + (g - 12.0f) * k; }
            float coord(float g) const
            {
                const float v = raw(g);
                if (!snap)
                    return v;
                return oddStroke ? std::floor(v) + 0.5f : std::round(v);
            }
            V P(float x, float y) const { return {coord(x), coord(y)}; }
            float w() const { return width > 0 ? width : stroke; }

            float k = 1, center = 0, stroke = 1, width = 0;
            int size = 0;
            bool snap = false, oddStroke = true;
            std::vector<Shape> shapes;
        };

        // The icons (theme::icon), each on the 24 grid with the design inside 2..22.
        bool design(char32_t cp, Canvas &c)
        {
            switch (cp)
            {
            case 0xE768: // play
                c.polyline({{7.5f, 4.5f}, {19.5f, 12}, {7.5f, 19.5f}}, true);
                return true;
            case 0xE769: // play filled (PLAY GAME mockup button)
                c.polygon({{6.5f, 4.0f}, {20.0f, 12.0f}, {6.5f, 20.0f}});
                return true;
            case 0xE910: // crossed wrench and screwdriver (BUILD GAME mockup button)
                // Screwdriver (top-left to bottom-right)
                c.weight(1.6f);
                c.line(5.0f, 5.0f, 14.0f, 14.0f);
                c.weight(2.8f);
                c.line(14.5f, 14.5f, 19.2f, 19.2f);
                c.weight(1.3f);
                c.line(4.0f, 6.0f, 6.0f, 4.0f);
                // Wrench (bottom-left to top-right)
                c.weight(2.0f);
                c.line(5.2f, 18.8f, 14.6f, 9.4f);
                c.weight(1.6f);
                c.arc(17.0f, 7.0f, 3.3f, 25.0f, 260.0f);
                c.disc(5.2f, 18.8f, 1.6f);
                return true;
            case 0xE911: // Steam logo (ADD TO STEAM mockup button)
                // White filled outer circle with piston cutouts
                c.disc(12.0f, 12.0f, 10.0f, false);
                c.weight(2.2f);
                c.line(2.2f, 13.0f, 9.6f, 15.6f, true);
                c.weight(1.9f);
                c.line(9.6f, 15.6f, 15.2f, 9.0f, true);
                // Upper-right crank wheel
                c.disc(15.2f, 9.0f, 3.3f, true);
                c.disc(15.2f, 9.0f, 2.1f, false);
                c.disc(15.2f, 9.0f, 1.1f, true);
                // Lower-left pivot wheel
                c.disc(9.6f, 15.6f, 2.5f, true);
                c.disc(9.6f, 15.6f, 1.5f, false);
                c.disc(9.6f, 15.6f, 0.7f, true);
                return true;
            case 0xEA86: // Mod package / puzzle icon
                c.box(4.5f, 6.5f, 19.5f, 19.5f, 2.0f);
                c.line(4.5f, 11.0f, 19.5f, 11.0f);
                c.line(12.0f, 6.5f, 12.0f, 11.0f);
                return true;

            case 0xE713: // settings: a cog
                c.circle(12, 12, 3);
                c.circle(12, 12, 7);
                c.weight(1.9f);
                for (int i = 0; i < 8; ++i)
                {
                    const float a = float(i) * kPi / 4;
                    c.line(12 + 7.3f * std::cos(a), 12 + 7.3f * std::sin(a), 12 + 9.2f * std::cos(a), 12 + 9.2f * std::sin(a));
                }
                return true;
            case 0xE897: // help
                c.circle(12, 12, 9.5f);
                c.arc(12, 9.6f, 3, 195, 230);
                c.line(12 + 3 * std::cos(65 * kPi / 180), 9.6f + 3 * std::sin(65 * kPi / 180), 12, 13.6f);
                c.dot(12, 16.8f);
                return true;
            case 0xE74E: // save: a floppy disk
                c.polyline({{4.5f, 4.5f}, {16, 4.5f}, {19.5f, 8}, {19.5f, 19.5f}, {4.5f, 19.5f}}, true);
                c.polyline({{8, 4.5f}, {8, 9}, {15, 9}, {15, 4.5f}});
                c.polyline({{7.5f, 19.5f}, {7.5f, 14}, {16.5f, 14}, {16.5f, 19.5f}});
                return true;
            case 0xED25: // folder
                c.weight(1.25f);
                c.polyline({{3, 18.5f}, {3, 5.5f}, {9.5f, 5.5f}, {11.5f, 8}, {21, 8}, {21, 18.5f}}, true);
                return true;
            case 0xE8E5: // open file: a page with an arrow up
                c.polyline({{9, 20.5f}, {5.5f, 20.5f}, {5.5f, 3.5f}, {13.5f, 3.5f}, {18.5f, 8.5f}, {18.5f, 12}});
                c.polyline({{13.5f, 3.5f}, {13.5f, 8.5f}, {18.5f, 8.5f}});
                c.line(15, 21, 15, 14.5f);
                c.arrowhead(15, 14, 0, -1, 3.2f);
                return true;
            case 0xE958: // disc
                c.circle(12, 12, 9.5f);
                c.circle(12, 12, 2.5f);
                c.arc(12, 12, 6.2f, 200, 55);
                return true;
            case 0xE896: // download
                c.line(12, 3.5f, 12, 15.5f);
                c.polyline({{7, 10.5f}, {12, 15.5f}, {17, 10.5f}});
                c.line(5, 20, 19, 20);
                return true;
            case 0xE72C: // refresh
                c.arcArrow(12, 12, 7.8f, -30, 290, 3.6f);
                return true;
            case 0xE921: // minimise
                c.line(4.5f, 12, 19.5f, 12);
                return true;
            case 0xE922: // maximise
                c.box(4.5f, 4.5f, 19.5f, 19.5f, 1.5f);
                return true;
            case 0xE923: // restore
                c.box(4.5f, 8, 16, 19.5f, 1.5f);
                c.polyline({{8, 4.5f}, {18, 4.5f}, {19.5f, 6}, {19.5f, 16}});
                return true;
            case 0xE8BB: // close
                c.line(5, 5, 19, 19);
                c.line(19, 5, 5, 19);
                return true;
            case 0xE73E: // check
                c.polyline({{4.5f, 12.5f}, {9.5f, 17.5f}, {19.5f, 6.5f}});
                return true;
            case 0xE711: // cancel
                c.line(6, 6, 18, 18);
                c.line(18, 6, 6, 18);
                return true;
            case 0xE7BA: // warning
                c.polyline({{12, 3.5f}, {21, 19.5f}, {3, 19.5f}}, true);
                c.line(12, 9.5f, 12, 13.5f);
                c.dot(12, 16.6f);
                return true;
            case 0xE946: // info
                c.circle(12, 12, 9.5f);
                c.line(12, 10.8f, 12, 17);
                c.dot(12, 7.4f);
                return true;
            case 0xE72B: // back
                c.line(4.5f, 12, 19.5f, 12);
                c.polyline({{10.5f, 6}, {4.5f, 12}, {10.5f, 18}});
                return true;
            case 0xE76C: // chevron right
                c.polyline({{9, 5}, {16, 12}, {9, 19}});
                return true;
            case 0xE70D: // chevron down
                c.polyline({{5, 9}, {12, 16}, {19, 9}});
                return true;
            case 0xE8A5: // document
                c.polyline({{5.5f, 3.5f}, {13.5f, 3.5f}, {18.5f, 8.5f}, {18.5f, 20.5f}, {5.5f, 20.5f}}, true);
                c.polyline({{13.5f, 3.5f}, {13.5f, 8.5f}, {18.5f, 8.5f}});
                c.line(9, 13, 15, 13);
                c.line(9, 16.5f, 15, 16.5f);
                return true;
            case 0xE90F: // repair: a wrench
                c.arc(15.5f, 8.5f, 4.2f, 0, 270);
                c.line(15.5f, 8.5f - 4.2f, 15.5f, 7);
                c.line(15.5f + 4.2f, 8.5f, 17, 8.5f);
                c.weight(1.8f);
                c.line(12.6f, 11.4f, 5, 19);
                return true;
            case 0xE943: // code
                c.polyline({{8, 7}, {3, 12}, {8, 17}});
                c.polyline({{16, 7}, {21, 12}, {16, 17}});
                c.line(13.8f, 5, 10.2f, 19);
                return true;
            case 0xE950: // chip
                c.box(6.5f, 6.5f, 17.5f, 17.5f, 1.5f);
                c.fillBox(9.5f, 9.5f, 14.5f, 14.5f, 0.8f);
                for (float p : {9.5f, 14.5f})
                {
                    c.line(p, 3, p, 6.5f);
                    c.line(p, 17.5f, p, 21);
                    c.line(3, p, 6.5f, p);
                    c.line(17.5f, p, 21, p);
                }
                return true;
            case 0xE823: // clock
                c.circle(12, 12, 9.5f);
                c.polyline({{12, 6.5f}, {12, 12}, {15.5f, 14.5f}});
                return true;
            case 0xE895: // sync: two arrows round a circle
                c.arcArrow(12, 12, 7.8f, 200, 120, 3.2f);
                c.arcArrow(12, 12, 7.8f, 20, 120, 3.2f);
                return true;
            case 0xE71B: // link: two capsules in a chain
                c.stadium(6.2f, 17.8f, 10.2f, 13.8f, 3.4f);
                c.stadium(13.8f, 10.2f, 17.8f, 6.2f, 3.4f);
                c.line(9.6f, 14.4f, 14.4f, 9.6f);
                return true;
            case 0xE7A7: // undo
                c.polyline({{9, 4.5f}, {4.5f, 9}, {9, 13.5f}});
                c.line(4.5f, 9, 14, 9);
                c.arc(14, 14, 5, 270, 180);
                c.line(14, 19, 8, 19);
                return true;
            case 0xEBE8: // bug
                c.box(7.5f, 8.5f, 16.5f, 20.5f, 4.5f);
                c.arc(12, 8.5f, 3, 180, 180);
                c.line(12, 12, 12, 20);
                c.line(7.5f, 12.5f, 3.5f, 11);
                c.line(7.5f, 16.5f, 3.5f, 18);
                c.line(16.5f, 12.5f, 20.5f, 11);
                c.line(16.5f, 16.5f, 20.5f, 18);
                c.line(10.2f, 5.8f, 8.5f, 3.5f);
                c.line(13.8f, 5.8f, 15.5f, 3.5f);
                return true;
            case 0xEDA2: // drive
                c.box(3, 7, 21, 17, 2.5f);
                c.line(6.5f, 12, 12, 12);
                c.dot(17, 12);
                return true;
            case 0xE8A7: // open in a new window
                c.polyline({{10.5f, 4.5f}, {4.5f, 4.5f}, {4.5f, 19.5f}, {19.5f, 19.5f}, {19.5f, 13.5f}});
                c.polyline({{13.5f, 4.5f}, {19.5f, 4.5f}, {19.5f, 10.5f}});
                c.line(19.5f, 4.5f, 11, 13);
                return true;
            case 0xEA37: // list
                for (float y : {6.5f, 12.0f, 17.5f})
                {
                    c.dot(4.5f, y);
                    c.line(8.5f, y, 20, y);
                }
                return true;
            case 0xE7FC: // game: a controller
                c.box(2.5f, 7, 21.5f, 18, 5);
                c.line(6, 12.5f, 10, 12.5f);
                c.line(8, 10.5f, 8, 14.5f);
                c.dot(15.5f, 11.2f);
                c.dot(17.8f, 13.8f);
                return true;
            case 0xE71A: // stop
                c.box(6, 6, 18, 18, 2);
                return true;
            default:
                return false;
            }
        }
    }

    bool rasterize(char32_t codepoint, int px, raster::Bitmap &out)
    {
        Canvas canvas(std::max(px, 4));
        if (!design(codepoint, canvas))
            return false;
        canvas.render(out);
        return true;
    }
}
