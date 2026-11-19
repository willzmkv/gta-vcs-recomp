// See ui_theme.h.
#include "ui/ui_theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

namespace ui
{
    namespace
    {
        ImFont *g_fontRegular = nullptr;
        ImFont *g_fontSemibold = nullptr;

        std::string findFirstFont(const std::vector<std::filesystem::path> &dirs, std::initializer_list<const char *> names)
        {
            std::error_code ec;
            for (const char *name : names)
            {
                for (const std::filesystem::path &dir : dirs)
                {
                    const std::filesystem::path direct = dir / name;
                    if (std::filesystem::is_regular_file(direct, ec))
                        return direct.string();
                }
            }
            return {};
        }

        std::vector<std::filesystem::path> fontSearchDirs()
        {
            std::vector<std::filesystem::path> dirs;
            std::error_code ec;
            auto addIfDir = [&](const std::filesystem::path &p) {
                if (!p.empty() && std::filesystem::is_directory(p, ec))
                    dirs.push_back(p);
            };
#if defined(_WIN32)
            if (const char *localApp = std::getenv("LOCALAPPDATA"))
                addIfDir(std::filesystem::path(localApp) / "Microsoft" / "Windows" / "Fonts");
            if (const char *winDir = std::getenv("WINDIR"))
                addIfDir(std::filesystem::path(winDir) / "Fonts");
            else
                addIfDir("C:/Windows/Fonts");
#else
            std::vector<std::filesystem::path> roots;
            if (const char *home = std::getenv("HOME"))
                roots.push_back(std::filesystem::path(home) / ".local" / "share" / "fonts");
            roots.push_back("/usr/local/share/fonts");
            roots.push_back("/usr/share/fonts");
            for (const std::filesystem::path &root : roots)
            {
                if (!std::filesystem::is_directory(root, ec))
                    continue;
                dirs.push_back(root);
                for (auto it = std::filesystem::recursive_directory_iterator(
                         root, std::filesystem::directory_options::skip_permission_denied, ec);
                     !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
                {
                    if (it.depth() > 3)
                    {
                        it.disable_recursion_pending();
                        continue;
                    }
                    if (it->is_directory(ec))
                        dirs.push_back(it->path());
                }
            }
#endif
            return dirs;
        }
    }

    void initTheme()
    {
        if (!ImGui::GetCurrentContext())
            return;

        ImGuiIO &io = ImGui::GetIO();
        // Keep ImGui's built-in bitmap font as Fonts[0] so the F3 debug overlay keeps its compact dark look,
        // while F2 and F5 push Inter / system UI Regular and SemiBold explicitly.
        if (io.Fonts->Fonts.empty())
            io.Fonts->AddFontDefault();

        const std::vector<std::filesystem::path> dirs = fontSearchDirs();
#if defined(_WIN32)
        const std::string regPath = findFirstFont(
            dirs, {"Inter-Regular.ttf", "Inter_18pt-Regular.ttf", "Inter_24pt-Regular.ttf", "segoeui.ttf", "arial.ttf"});
        const std::string semiPath = findFirstFont(
            dirs, {"Inter-SemiBold.ttf", "Inter_18pt-SemiBold.ttf", "Inter_24pt-SemiBold.ttf", "seguisb.ttf", "segoeuib.ttf",
                   "arialbd.ttf"});
#else
        const std::string regPath = findFirstFont(
            dirs, {"Inter-Regular.ttf", "Inter_18pt-Regular.ttf", "Cantarell-Regular.otf", "NotoSans-Regular.ttf",
                   "Ubuntu-Regular.ttf", "Ubuntu-R.ttf", "DejaVuSans.ttf", "LiberationSans-Regular.ttf"});
        const std::string semiPath = findFirstFont(
            dirs, {"Inter-SemiBold.ttf", "Inter_18pt-SemiBold.ttf", "Cantarell-Bold.otf", "NotoSans-SemiBold.ttf",
                   "NotoSans-Bold.ttf", "Ubuntu-Medium.ttf", "Ubuntu-B.ttf", "DejaVuSans-Bold.ttf", "LiberationSans-Bold.ttf"});
#endif

        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        cfg.PixelSnapH = true;

        if (!regPath.empty())
            g_fontRegular = io.Fonts->AddFontFromFileTTF(regPath.c_str(), 16.0f, &cfg);
        if (!semiPath.empty())
            g_fontSemibold = io.Fonts->AddFontFromFileTTF(semiPath.c_str(), 16.0f, &cfg);
        if (!g_fontSemibold)
            g_fontSemibold = g_fontRegular;

        std::fprintf(stderr, "[ui] theme fonts: regular=%s semibold=%s\n", regPath.empty() ? "(default)" : regPath.c_str(),
                     semiPath.empty() ? "(fallback)" : semiPath.c_str());
    }

    ImFont *fontRegular() { return g_fontRegular ? g_fontRegular : ImGui::GetFont(); }
    ImFont *fontSemibold() { return g_fontSemibold ? g_fontSemibold : fontRegular(); }

    void drawChevron(ImDrawList *dl, ImVec2 c, bool rightDir, float s, ImU32 col)
    {
        const float dx = 4.2f * s * (rightDir ? 1.0f : -1.0f);
        const float dy = 6.8f * s;
        const float thick = std::max(1.6f, 2.2f * s);
        const ImVec2 pts[3] = {{c.x - dx, c.y - dy}, {c.x + dx, c.y}, {c.x - dx, c.y + dy}};
        dl->AddPolyline(pts, 3, col, 0, thick);
    }

    void drawPageIcon(ImDrawList *dl, const std::string &page, ImVec2 c, float size, ImU32 col)
    {
        const float r = size * 0.5f;
        const float t = std::max(1.3f, size * 0.11f);

        if (page == "Display")
        {
            dl->AddRect(ImVec2(c.x - r * 0.85f, c.y - r * 0.65f), ImVec2(c.x + r * 0.85f, c.y + r * 0.35f), col,
                        size * 0.14f, 0, t);
            dl->AddLine(ImVec2(c.x, c.y + r * 0.35f), ImVec2(c.x, c.y + r * 0.72f), col, t);
            dl->AddLine(ImVec2(c.x - r * 0.45f, c.y + r * 0.72f), ImVec2(c.x + r * 0.45f, c.y + r * 0.72f), col, t);
        }
        else if (page == "Graphics")
        {
            dl->AddRect(ImVec2(c.x - r * 0.82f, c.y - r * 0.68f), ImVec2(c.x + r * 0.82f, c.y + r * 0.68f), col,
                        size * 0.16f, 0, t);
            dl->AddCircleFilled(ImVec2(c.x - r * 0.32f, c.y - r * 0.22f), std::max(1.3f, size * 0.085f), col);
            const ImVec2 ridge[4] = {
                {c.x - r * 0.68f, c.y + r * 0.48f},
                {c.x - r * 0.12f, c.y + r * 0.02f},
                {c.x + r * 0.20f, c.y + r * 0.28f},
                {c.x + r * 0.68f, c.y - r * 0.12f},
            };
            dl->AddPolyline(ridge, 4, col, 0, t);
        }
        else if (page == "Audio")
        {
            const ImVec2 poly[6] = {
                {c.x - r * 0.78f, c.y - r * 0.26f}, {c.x - r * 0.35f, c.y - r * 0.26f}, {c.x + r * 0.05f, c.y - r * 0.62f},
                {c.x + r * 0.05f, c.y + r * 0.62f}, {c.x - r * 0.35f, c.y + r * 0.26f}, {c.x - r * 0.78f, c.y + r * 0.26f},
            };
            dl->AddPolyline(poly, 6, col, ImDrawFlags_Closed, t);
            dl->PathArcTo(ImVec2(c.x + r * 0.12f, c.y), r * 0.36f, -0.75f, 0.75f, 8);
            dl->PathStroke(col, 0, t);
            dl->PathArcTo(ImVec2(c.x + r * 0.12f, c.y), r * 0.68f, -0.82f, 0.82f, 10);
            dl->PathStroke(col, 0, t);
        }
        else if (page == "Keyboard")
        {
            dl->AddRect(ImVec2(c.x - r * 0.88f, c.y - r * 0.52f), ImVec2(c.x + r * 0.88f, c.y + r * 0.52f), col,
                        size * 0.14f, 0, t);
            const float dot = std::max(1.0f, size * 0.065f);
            for (int i = -2; i <= 2; ++i)
                dl->AddCircleFilled(ImVec2(c.x + i * r * 0.28f, c.y - r * 0.16f), dot, col);
            dl->AddLine(ImVec2(c.x - r * 0.42f, c.y + r * 0.20f), ImVec2(c.x + r * 0.42f, c.y + r * 0.20f), col, t);
        }
        else if (page == "Gamepad")
        {
            dl->AddRect(ImVec2(c.x - r * 0.88f, c.y - r * 0.48f), ImVec2(c.x + r * 0.88f, c.y + r * 0.48f), col,
                        r * 0.45f, 0, t);
            const float dx = c.x - r * 0.38f;
            dl->AddLine(ImVec2(dx - r * 0.20f, c.y), ImVec2(dx + r * 0.20f, c.y), col, t);
            dl->AddLine(ImVec2(dx, c.y - r * 0.20f), ImVec2(dx, c.y + r * 0.20f), col, t);
            const float bd = std::max(1.1f, size * 0.075f);
            dl->AddCircleFilled(ImVec2(c.x + r * 0.32f, c.y + r * 0.10f), bd, col);
            dl->AddCircleFilled(ImVec2(c.x + r * 0.52f, c.y - r * 0.10f), bd, col);
        }
        else if (page == "Enhancements")
        {
            const ImVec2 star[8] = {
                {c.x, c.y - r * 0.82f},         {c.x + r * 0.22f, c.y - r * 0.22f}, {c.x + r * 0.82f, c.y},
                {c.x + r * 0.22f, c.y + r * 0.22f}, {c.x, c.y + r * 0.82f},         {c.x - r * 0.22f, c.y + r * 0.22f},
                {c.x - r * 0.82f, c.y},         {c.x - r * 0.22f, c.y - r * 0.22f},
            };
            dl->AddPolyline(star, 8, col, ImDrawFlags_Closed, t);
        }
        else if (page == "Mods")
        {
            const ImVec2 top[4] = {
                {c.x, c.y - r * 0.72f},
                {c.x + r * 0.78f, c.y - r * 0.22f},
                {c.x, c.y + r * 0.24f},
                {c.x - r * 0.78f, c.y - r * 0.22f},
            };
            dl->AddPolyline(top, 4, col, ImDrawFlags_Closed, t);
            const ImVec2 mid[3] = {{c.x - r * 0.74f, c.y + r * 0.14f}, {c.x, c.y + r * 0.54f}, {c.x + r * 0.74f, c.y + r * 0.14f}};
            dl->AddPolyline(mid, 3, col, 0, t);
            const ImVec2 bot[3] = {{c.x - r * 0.74f, c.y + r * 0.44f}, {c.x, c.y + r * 0.82f}, {c.x + r * 0.74f, c.y + r * 0.44f}};
            dl->AddPolyline(bot, 3, col, 0, t);
        }
        else if (page == "Game")
        {
            dl->AddRect(ImVec2(c.x - r * 0.76f, c.y - r * 0.76f), ImVec2(c.x + r * 0.76f, c.y + r * 0.76f), col,
                        size * 0.18f, 0, t);
            dl->AddTriangleFilled(ImVec2(c.x - r * 0.22f, c.y - r * 0.34f), ImVec2(c.x + r * 0.38f, c.y),
                                  ImVec2(c.x - r * 0.22f, c.y + r * 0.34f), col);
        }
        else if (page == "Debug")
        {
            dl->AddRect(ImVec2(c.x - r * 0.82f, c.y - r * 0.66f), ImVec2(c.x + r * 0.82f, c.y + r * 0.66f), col,
                        size * 0.14f, 0, t);
            const ImVec2 chev[3] = {{c.x - r * 0.46f, c.y - r * 0.26f}, {c.x - r * 0.12f, c.y}, {c.x - r * 0.46f, c.y + r * 0.26f}};
            dl->AddPolyline(chev, 3, col, 0, t);
            dl->AddLine(ImVec2(c.x + r * 0.05f, c.y + r * 0.26f), ImVec2(c.x + r * 0.46f, c.y + r * 0.26f), col, t);
        }
        else
        {
            for (int i = -1; i <= 1; ++i)
            {
                const float y = c.y + i * r * 0.50f;
                const float kx = c.x + (i == 0 ? r * 0.28f : -r * 0.22f);
                dl->AddLine(ImVec2(c.x - r * 0.76f, y), ImVec2(c.x + r * 0.76f, y), col, t);
                dl->AddCircleFilled(ImVec2(kx, y), std::max(1.6f, size * 0.11f), col);
            }
        }
    }
}
