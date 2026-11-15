#pragma once

#include <filesystem>

namespace sound
{
    enum class Sfx
    {
        Nav,   // navegando entre menus.wav
        Back,  // voltar.wav
        Play,  // clicando em jogar.wav
    };

    void init(const std::filesystem::path &assetsDir);
    void play(Sfx sfx);
    void shutdown();
}
