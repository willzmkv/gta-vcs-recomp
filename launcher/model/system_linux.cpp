// The launcher's OS pieces on Linux (model/system.h): XDG folders, xdg-open, the file manager over D-Bus
// (org.freedesktop.FileManager1), .desktop shortcuts, logind's sleep inhibitor.
#include "system.h"

#include "platform/linux/dbus.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace launcher
{
    namespace
    {
        fs::path home()
        {
            if (const char *h = std::getenv("HOME"); h && *h)
                return pathFrom(h);
            return "/tmp";
        }

        fs::path xdgDir(const char *variable, const char *fallback)
        {
            if (const char *v = std::getenv(variable); v && *v == '/')
                return pathFrom(v);
            return home() / fallback;
        }

        // XDG_DESKTOP_DIR="$HOME/Desktop" from user-dirs.dirs (xdg-user-dirs; localised names)
        fs::path userDir(const char *key, const char *fallback)
        {
            std::ifstream in(xdgDir("XDG_CONFIG_HOME", ".config") / "user-dirs.dirs");
            std::string line;
            const std::string prefix = std::string(key) + "=";
            while (std::getline(in, line))
            {
                if (line.rfind(prefix, 0) != 0)
                    continue;
                std::string value = line.substr(prefix.size());
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                    value = value.substr(1, value.size() - 2);
                if (value.rfind("$HOME", 0) == 0)
                    return home() / pathFrom(value.substr(value.size() > 5 && value[5] == '/' ? 6 : 5));
                if (!value.empty() && value[0] == '/')
                    return pathFrom(value);
            }
            return home() / fallback;
        }

        // a program started on its own: a grandchild in a session of its own, so it neither becomes a zombie of ours
        // nor dies with the launcher. True when it could be started.
        bool spawnDetached(const std::vector<std::string> &argv)
        {
            std::vector<char *> args;
            for (const auto &a : argv)
                args.push_back(const_cast<char *>(a.c_str()));
            args.push_back(nullptr);
            int status[2];
            if (::pipe2(status, O_CLOEXEC) != 0)
                return false;
            const pid_t pid = ::fork();
            if (pid == 0)
            {
                ::setsid();
                if (::fork() != 0)
                    _exit(0);
                const int nul = ::open("/dev/null", O_RDWR);
                if (nul >= 0)
                {
                    ::dup2(nul, 0);
                    ::dup2(nul, 1);
                    ::dup2(nul, 2);
                }
                closeInheritedFdsOnExec();
                ::execvp(args[0], args.data());
                const int err = errno;
                [[maybe_unused]] ssize_t n = ::write(status[1], &err, sizeof(err));
                _exit(127);
            }
            ::close(status[1]);
            if (pid < 0)
            {
                ::close(status[0]);
                return false;
            }
            int st;
            ::waitpid(pid, &st, 0);
            int err = 0;
            const ssize_t got = ::read(status[0], &err, sizeof(err));
            ::close(status[0]);
            return got != sizeof(err);
        }

        std::string fileUri(const fs::path &p)
        {
            static const char *hex = "0123456789ABCDEF";
            std::string out = "file://";
            for (unsigned char c : pathText(fs::absolute(p)))
            {
                if (std::isalnum(c) || std::strchr("/-_.~", c))
                    out += char(c);
                else
                {
                    out += '%';
                    out += hex[c >> 4];
                    out += hex[c & 15];
                }
            }
            return out;
        }

        // a value in a .desktop file's Exec line: quoted, with the characters the spec reserves escaped
        std::string desktopExecArg(const std::string &s)
        {
            std::string out = "\"";
            for (char c : s)
            {
                if (c == '"' || c == '`' || c == '$' || c == '\\')
                    out += "\\\\";
                if (c == '%')
                    out += '%';
                out += c;
            }
            return out + "\"";
        }

        int g_inhibitFd = -1;
    }

    std::string lastErrorText(unsigned long code) { return std::strerror(int(code)); }

    fs::path exePath()
    {
        std::error_code ec;
        fs::path p = fs::read_symlink("/proc/self/exe", ec);
        return ec ? fs::path() : p;
    }

    fs::path knownFolder(Folder which)
    {
        switch (which)
        {
        case Folder::Desktop: return userDir("XDG_DESKTOP_DIR", "Desktop");
        case Folder::Downloads: return userDir("XDG_DOWNLOAD_DIR", "Downloads");
        case Folder::Documents: return userDir("XDG_DOCUMENTS_DIR", "Documents");
        case Folder::LocalAppData: return xdgDir("XDG_DATA_HOME", ".local/share");
        }
        return home();
    }

    void openFolder(const fs::path &dir)
    {
        std::error_code ec;
        fs::create_directories(dir, ec);
        spawnDetached({"xdg-open", pathText(dir)});
    }

    void revealFile(const fs::path &file)
    {
        // the file manager with the file selected (Nautilus, Dolphin, Nemo, Thunar...); else its folder
        const std::string uri = fileUri(file);
        const char *uris[] = {uri.c_str()};
        if (!dbus::showItems(uris, 1))
            openFolder(file.parent_path());
    }

    void openUrl(const std::string &url) { spawnDetached({"xdg-open", url}); }

    void openFile(const fs::path &file) { spawnDetached({"xdg-open", pathText(file)}); }

    bool createShortcut(const fs::path &link, const fs::path &target, const fs::path &workingDir,
                        const std::string &description, std::string &error)
    {
        const fs::path icon = workingDir / "launcher" / "launcher.png";
        std::string entry = "[Desktop Entry]\n"
                            "Type=Application\n"
                            "Name=GTA: Vice City Stories\n"
                            "Comment=" + description + "\n"
                            "Exec=" + desktopExecArg(pathText(target)) + "\n"
                            "Path=" + pathText(workingDir) + "\n";
        std::error_code ec;
        if (fs::is_regular_file(icon, ec))
            entry += "Icon=" + pathText(icon) + "\n";
        entry += "Terminal=false\n"
                 "Categories=Game;\n"
                 "StartupWMClass=VCSLauncher\n";
        // the Desktop (the link asked for) and the applications menu
        const fs::path menu = knownFolder(Folder::LocalAppData) / "applications" / link.filename();
        bool wrote = false;
        for (const fs::path &file : {link, menu})
        {
            fs::create_directories(file.parent_path(), ec);
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << entry;
            out.close();
            if (!out)
            {
                if (file == link)
                {
                    error = "cannot write " + pathText(file);
                    return false;
                }
                continue;
            }
            ::chmod(pathText(file).c_str(), 0755);
            wrote = true;
        }
        // GNOME / XFCE ask before running a launcher on the Desktop unless it is marked trusted
        spawnDetached({"gio", "set", pathText(link), "metadata::trusted", "true"});
        return wrote;
    }

    void openTerminal(const std::string &command)
    {
        // the user's terminal ($TERMINAL, Debian's alternative), else the desktops' own, else xterm
        const std::string script = command + "; echo; echo 'Done. Press Enter to close, then Check again in the launcher.'; read _";
        const std::vector<std::vector<std::string>> terminals = {
            {"x-terminal-emulator", "-e", "sh", "-c", script},
            {"gnome-terminal", "--", "sh", "-c", script},
            {"kgx", "--", "sh", "-c", script},
            {"konsole", "-e", "sh", "-c", script},
            {"xfce4-terminal", "-x", "sh", "-c", script},
            {"mate-terminal", "-x", "sh", "-c", script},
            {"alacritty", "-e", "sh", "-c", script},
            {"kitty", "sh", "-c", script},
            {"foot", "sh", "-c", script},
            {"xterm", "-e", "sh", "-c", script},
        };
        if (const char *t = std::getenv("TERMINAL"); t && *t && spawnDetached({t, "-e", "sh", "-c", script}))
            return;
        for (const auto &argv : terminals)
            if (spawnDetached(argv))
                return;
    }

    void keepAwake(bool on)
    {
        // logind holds the inhibitor while we hold its file descriptor
        if (on && g_inhibitFd < 0)
            g_inhibitFd = dbus::inhibitSleep("VCS Launcher", "Building the game");
        else if (!on && g_inhibitFd >= 0)
        {
            ::close(g_inhibitFd);
            g_inhibitFd = -1;
        }
    }

    bool attachParentConsole() { return ::isatty(1) != 0; } // a terminal's output is ours already

    bool redirectOutput(const fs::path &file)
    {
        const int fd = ::open(pathText(file).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            return false;
        std::fflush(stdout);
        std::fflush(stderr);
        ::dup2(fd, 1);
        ::dup2(fd, 2);
        ::close(fd);
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        return true;
    }

    uint64_t freeBytes(const fs::path &dir)
    {
        struct statvfs s{};
        if (::statvfs(pathText(dir).c_str(), &s) != 0)
            return 0;
        return uint64_t(s.f_bavail) * s.f_frsize;
    }

    double processCpuSeconds()
    {
        rusage u{};
        ::getrusage(RUSAGE_SELF, &u);
        auto seconds = [](const timeval &t) { return double(t.tv_sec) + double(t.tv_usec) / 1e6; };
        return seconds(u.ru_utime) + seconds(u.ru_stime);
    }

    unsigned long processId() { return static_cast<unsigned long>(::getpid()); }

    void closeInheritedFdsOnExec()
    {
        // close_range(CLOSE_RANGE_CLOEXEC): Linux 5.11. Marked, not closed: the child's exec-error pipe (O_CLOEXEC
        // already) must still work if exec fails. Older kernels: each fd up to the limit
        constexpr unsigned kCloseRangeCloexec = 1u << 2;
#ifdef SYS_close_range
        if (::syscall(SYS_close_range, 3u, ~0u, kCloseRangeCloexec) == 0)
            return;
#else
        (void)kCloseRangeCloexec;
#endif
        rlimit lim{};
        const long top = ::getrlimit(RLIMIT_NOFILE, &lim) == 0 && lim.rlim_cur != RLIM_INFINITY ? long(lim.rlim_cur) : 4096;
        for (int fd = 3; fd < top && fd < 65536; ++fd)
            if (const int flags = ::fcntl(fd, F_GETFD); flags >= 0 && !(flags & FD_CLOEXEC))
                ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }

    void waitForProcessExit(unsigned long pid, double seconds)
    {
        // not our child: polled. A zombie (ended, its parent not reaping it yet) counts as gone
        const std::string stat = "/proc/" + std::to_string(pid) + "/stat";
        for (double waited = 0; waited < seconds; waited += 0.05)
        {
            std::ifstream in(stat);
            std::string line;
            if (!std::getline(in, line))
                return;
            const size_t paren = line.rfind(')'); // "pid (name) S ...": the name may hold spaces and brackets
            if (paren == std::string::npos || paren + 2 >= line.size() || line[paren + 2] == 'Z' || line[paren + 2] == 'X')
                return;
            ::usleep(50 * 1000);
        }
    }
}
