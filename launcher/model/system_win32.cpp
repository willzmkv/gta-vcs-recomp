// The launcher's OS pieces on Windows (model/system.h).
#include "system.h"

#include <windows.h>
#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>

#include <cstdio>
#include <fcntl.h>
#include <io.h>

namespace launcher
{
    std::wstring widen(const std::string &utf8)
    {
        if (utf8.empty())
            return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
        std::wstring out(size_t(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), n);
        return out;
    }

    std::string narrow(const std::wstring &wide)
    {
        if (wide.empty())
            return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
        std::string out(size_t(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    std::string lastErrorText(unsigned long code)
    {
        wchar_t *text = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                       code, 0, reinterpret_cast<wchar_t *>(&text), 0, nullptr);
        std::string out = text ? narrow(text) : "error " + std::to_string(code);
        if (text)
            LocalFree(text);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '.'))
            out.pop_back();
        return out;
    }

    std::filesystem::path exePath()
    {
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
            if (n < buffer.size())
            {
                buffer.resize(n);
                return buffer;
            }
            buffer.resize(buffer.size() * 2);
        }
    }

    std::filesystem::path knownFolder(Folder which)
    {
        const KNOWNFOLDERID *id = &FOLDERID_Desktop;
        switch (which)
        {
        case Folder::Desktop: id = &FOLDERID_Desktop; break;
        case Folder::Downloads: id = &FOLDERID_Downloads; break;
        case Folder::Documents: id = &FOLDERID_Documents; break;
        case Folder::LocalAppData: id = &FOLDERID_LocalAppData; break;
        }
        PWSTR path = nullptr;
        std::filesystem::path out;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, 0, nullptr, &path)))
            out = path;
        CoTaskMemFree(path);
        return out;
    }

    void openFolder(const std::filesystem::path &dir)
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void revealFile(const std::filesystem::path &file)
    {
        const std::wstring args = L"/select,\"" + file.wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }

    void openUrl(const std::string &url)
    {
        ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void openFile(const std::filesystem::path &file)
    {
        ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    bool createShortcut(const std::filesystem::path &link, const std::filesystem::path &target,
                        const std::filesystem::path &workingDir, const std::string &description, std::string &error)
    {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IShellLinkW *shell = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                      reinterpret_cast<void **>(&shell));
        if (SUCCEEDED(hr))
        {
            shell->SetPath(target.c_str());
            shell->SetWorkingDirectory(workingDir.c_str());
            shell->SetDescription(widen(description).c_str());
            shell->SetIconLocation(target.c_str(), 0);
            IPersistFile *file = nullptr;
            hr = shell->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&file));
            if (SUCCEEDED(hr))
            {
                hr = file->Save(link.c_str(), TRUE);
                file->Release();
            }
            shell->Release();
        }
        if (SUCCEEDED(init))
            CoUninitialize();
        if (FAILED(hr))
            error = lastErrorText(static_cast<unsigned long>(hr));
        return SUCCEEDED(hr);
    }

    void openTerminal(const std::string &command)
    {
        const std::wstring args = L"/k " + widen(command);
        ShellExecuteW(nullptr, L"open", L"cmd.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }

    void keepAwake(bool on)
    {
        SetThreadExecutionState(on ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED) : ES_CONTINUOUS);
    }

    bool attachParentConsole()
    {
        if (!AttachConsole(ATTACH_PARENT_PROCESS))
            return false;
        FILE *f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        SetConsoleOutputCP(CP_UTF8);
        return true;
    }

    bool redirectOutput(const std::filesystem::path &file)
    {
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return false;
        // the Win32 handles (children inherit them) and the C runtime's stdout / stderr on the same file
        SetStdHandle(STD_OUTPUT_HANDLE, h);
        SetStdHandle(STD_ERROR_HANDLE, h);
        const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_WRONLY | _O_BINARY);
        if (fd < 0)
            return false;
        FILE *f = nullptr;
        freopen_s(&f, "NUL", "w", stdout);
        freopen_s(&f, "NUL", "w", stderr);
        _dup2(fd, _fileno(stdout));
        _dup2(fd, _fileno(stderr));
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        return true;
    }

    uint64_t freeBytes(const std::filesystem::path &dir)
    {
        ULARGE_INTEGER free{};
        if (!GetDiskFreeSpaceExW(dir.c_str(), &free, nullptr, nullptr))
            return 0;
        return free.QuadPart;
    }

    double processCpuSeconds()
    {
        FILETIME created, exited, kernel, user;
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        auto seconds = [](const FILETIME &f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7; };
        return seconds(kernel) + seconds(user);
    }

    unsigned long processId() { return GetCurrentProcessId(); }

    void waitForProcessExit(unsigned long pid, double seconds)
    {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
        if (!process)
            return; // gone already
        WaitForSingleObject(process, DWORD(seconds * 1000.0));
        CloseHandle(process);
    }
}
