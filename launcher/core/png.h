// PNG reader (via stb_image) and writer for screenshots (--screenshot): RGBA8.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace core
{
    bool writePng(const std::filesystem::path &path, int width, int height, const uint8_t *rgba);
    bool readPng(const std::filesystem::path &path, int &width, int &height, std::vector<uint8_t> &rgba);
    void resizeRgba(const uint8_t *src, int srcW, int srcH, uint8_t *dst, int dstW, int dstH);
}
