// OS pieces the launcher needs: text conversion, folders, the file manager, shortcuts, keeping the PC awake.
// system_win32.cpp (Windows) and system_linux.cpp implement them; the text that crosses this interface is UTF-8.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace launcher
{
#ifdef _WIN32
    std::wstring widen(const std::string &utf8);
    std::string narrow(const std::wstring &wide);
#endif
    std::string lastErrorText(unsigned long code); // GetLastError() / errno as text
    inline std::string pathText(const std::filesystem::path &p)
    {
        const std::u8string u = p.u8string();
        return std::string(u.begin(), u.end());
    }
    inline std::filesystem::path pathFrom(const std::string &utf8)
    {
        return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
    }

    std::filesystem::path exePath();
    enum class Folder
    {
        Desktop,
        Downloads,
        Documents,
        LocalAppData // Linux: $XDG_DATA_HOME (~/.local/share), where the game keeps its saves too
    };
    std::filesystem::path knownFolder(Folder which);

    void openFolder(const std::filesystem::path &dir);   // the file manager at the folder
    void revealFile(const std::filesystem::path &file);  // the file manager with the file selected
    void openUrl(const std::string &url);
    void openFile(const std::filesystem::path &file);     // its default program (a log in a text editor)
    // Windows: a .lnk at `link`. Linux: a .desktop entry at `link` (its folder: the Desktop) and in the applications
    // menu, with the launcher's icon
    bool createShortcut(const std::filesystem::path &link, const std::filesystem::path &target,
                        const std::filesystem::path &workingDir, const std::string &description, std::string &error);
    void keepAwake(bool on);                              // no sleep while a build runs
    // Linux: a terminal running this shell command, kept open at the end (a package install that asks for a password)
    void openTerminal(const std::string &command);
    bool attachParentConsole();                           // CLI mode: print into the terminal that started us
    bool redirectOutput(const std::filesystem::path &file); // CLI mode --out: our output and the tools' to a file
    uint64_t freeBytes(const std::filesystem::path &dir);
    double processCpuSeconds();                           // user + kernel time of this process
    unsigned long processId();                            // this process's id
    // block until process `pid` has ended, at most `seconds`: a launcher restarted after an update waits for the one
    // that started it (its renamed exe can only be removed, an AppImage's mount only goes, once it is gone)
    void waitForProcessExit(unsigned long pid, double seconds);
#ifndef _WIN32
    // in a forked child before exec: every fd above stderr closes on exec. Without it a child gets the fds we were
    // given without O_CLOEXEC - an AppImage runtime's keep-alive pipe among them, so a launcher started by Restart (or
    // the game, a browser) kept the old image mounted until it ended. Async-signal-safe
    void closeInheritedFdsOnExec();
#endif
}
