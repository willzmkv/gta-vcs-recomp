// VCS disc streaming without the IOP (docs/NATIVE_PORT_ROADMAP.md milestone 6 step 1).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class PS2Runtime;

namespace vcs
{
    // Game files from mods (the mod loader, docs/MODS.md "Game files"): GTA3PS2.IMG entries read from a mod's file
    // instead of the disc, only that entry - the archive itself is never rebuilt. `name`: the entry's model name (the
    // file's name without its extension; the game keeps ~CRC32 of it in upper case), a texture dictionary's name
    // (`textures`: `<name>.xtx`, the game's TXD store keeps the names) or "#<streaming index>" (tools/img.py); with a dot,
    // a loose disc file (AMERICAN.GXT). A file larger than its entry is read from past the disc. Any thread; resolved
    // against the game's tables at the next read; entries already in memory change when the game streams them again.
    // PC VC files are converted on the way (src/mods/rwconv.h): a `.dff` onto the disc's model of its name (with
    // `companion`, its `.txd`, for the texture names), a `.txd` into the texture dictionary of its name.
    struct StreamFile
    {
        enum class Format : uint8_t
        {
            Vcs,   // the game's own format, as it is
            PcDff, // a PC VC car model
            PcTxd, // a PC VC texture dictionary
        };
        std::string name, path, mod;
        int priority = 50;
        bool textures = false; // a texture dictionary (by the TXD store's names)
        Format format = Format::Vcs;
        std::string companion;
    };
    void setStreamFiles(std::vector<StreamFile> files);

    // The EE side of CDSTREAM.IRX's stream server (SIF RPC 0x65686577): read / poll / fn 5 replaced by reads from the
    // disc image on a host thread straight into EE RAM, the server's init call answered on the host. False with
    // VCS_NATIVE_STREAM=0, which keeps the IOP's (the reference).
    bool installStreamHooks(PS2Runtime &runtime);

    // Milestone 6 step 3: with streaming and sound both native the emulated IOP is switched off - no IRX module is
    // loaded or run - unless PS2X_IOP=1; PS2X_IOP=0 switches it off regardless.
    void retireIop(bool nativeComplete);
}
