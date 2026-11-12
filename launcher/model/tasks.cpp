#include "tasks.h"

#include "os.h"
#include "system.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdlib>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace launcher
{
    namespace
    {
        constexpr const char *kEventPrefix = "@vcs "; // tools/toolchain.py EVENT_PREFIX
        // what the launcher runs (tools/release.py's self-test makes sure a release has them)
        constexpr const char *kTools[] = {"doctor.py", "build.py", "toolchain.py", "recompile.py", "vcs_disc.py",
                                          "setup_vs.py", "backup.py", "report.py", "update.py", "rip_disc.py"};
        constexpr size_t kLogLines = 4000;

        std::string findOnPath(const char *name)
        {
#ifdef _WIN32
            wchar_t buffer[MAX_PATH * 2];
            const DWORD n = SearchPathW(nullptr, widen(name).c_str(), nullptr, DWORD(std::size(buffer)), buffer, nullptr);
            if (n == 0 || n >= std::size(buffer))
                return {};
            return narrow(buffer);
#else
            const char *path = std::getenv("PATH");
            std::string_view rest = path ? path : "/usr/local/bin:/usr/bin:/bin";
            while (!rest.empty())
            {
                const size_t colon = rest.find(':');
                const std::string_view dir = rest.substr(0, colon);
                rest = colon == std::string_view::npos ? std::string_view() : rest.substr(colon + 1);
                if (dir.empty())
                    continue;
                const fs::path candidate = pathFrom(std::string(dir)) / name;
                std::error_code ec;
                if (fs::is_regular_file(candidate, ec) &&
                    (fs::status(candidate, ec).permissions() & fs::perms::others_exec) != fs::perms::none)
                    return pathText(candidate);
            }
            return {};
#endif
        }

        std::string toolPath(const fs::path &root, const char *name) { return pathText(root / "tools" / name); }

        std::string readFile(const fs::path &path)
        {
            std::ifstream in(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), {});
        }

        std::string trim(std::string s)
        {
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
                s.pop_back();
            size_t i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
                ++i;
            return s.substr(i);
        }

        const std::map<std::string, ErrorHelp> kErrors = {
            {"no_vs", {"Visual Studio's C++ tools aren't installed",
                       "The game is compiled on your PC. Use \"Install build tools\" on the Build tools step (a free "
                       "download from Microsoft, about 6 GB)."}},
            {"no_cpp_tools", {"Visual Studio has no C++ tools",
                              "Use \"Install build tools\", or in the Visual Studio Installer choose Modify and tick "
                              "\"Desktop development with C++\" and \"C++ Clang tools for Windows\"."}},
            {"no_clang", {"The C++ Clang tools are missing",
                          "Use \"Add the Clang tools\", or in the Visual Studio Installer choose Modify and tick "
                          "\"C++ Clang tools for Windows\"."}},
            {"old_clang", {"Visual Studio's clang is too old",
                           "Update Visual Studio: open the Visual Studio Installer and choose Update."}},
            {"no_cmake", {"CMake couldn't be found or downloaded",
                          "The build downloads CMake when none is installed, and that failed: check the internet "
                          "connection and build again, or install CMake (cmake.org) or Visual Studio's \"C++ CMake "
                          "tools for Windows\" (Visual Studio Installer, Modify)."}},
            {"vcvars_failed", {"Visual Studio's environment could not be set up",
                               "Repair Visual Studio from the Visual Studio Installer (More > Repair)."}},
            {"no_disc", {"No disc image", "Choose your GTA: Vice City Stories (USA) .iso on the Disc step."}},
            {"disc_not_found", {"The disc image is gone", "It was moved or deleted: choose it again on the Disc step."}},
            {"wrong_disc", {"This is not the supported disc",
                            "Only GTA: Vice City Stories for the USA (SLUS-21590) works. Choose another image."}},
            {"download_failed", {"A download failed",
                                 "The first build downloads libraries. Check the internet connection and try again "
                                 "(a release with a deps folder needs none)."}},
            {"configure_failed", {"Setting up the build failed", "Open the log for the details, or copy a report for help."}},
            {"disk_full", {"The drive is full", "Free some space (a build needs about 3 GB) and build again."}},
            {"path_too_long", {"A path is too long for Windows",
                               "Move the whole folder somewhere short, like C:\\Games\\VCS, and build again."}},
            {"file_locked", {"A file was locked",
                             "Often an antivirus scanning the build, or a cloud-synced folder. Build again; if it keeps "
                             "happening, add the folder to Windows Security's exclusions or move it out of OneDrive."}},
            {"out_of_memory", {"The PC ran out of memory",
                               "Close other programs and build again; it continues where it stopped."}},
            {"file_dates", {"Files are dated in the future",
                            "Some files in the folder are newer than the PC's clock, so the build keeps starting over. "
                            "Check that Windows' clock and time zone are right, then build again."}},
            {"build_failed", {"The build failed", "Open the log for the details, or copy a report for help."}},
            {"recompiler_failed", {"Recompiling the game failed", "Open the log for the details, or copy a report for help."}},
            {"cpu_unsupported", {"This processor is too old for the game",
                                 "The game needs a processor with SSE4.2: an Intel Core i (2008) or AMD Bulldozer (2011) "
                                 "or newer."}},
            {"illegal_instruction", {"A program used an instruction this processor doesn't have",
                                     "It was built for a newer processor. Build again: the build is set up for this "
                                     "PC's processor. If it happens again, copy a report for help."}},
            {"missing_dll", {"A program is missing a library",
                             "Install the latest Microsoft Visual C++ Redistributable (x64) from Microsoft, then build "
                             "again."}},
            {"elevation_denied", {"The installer wasn't allowed to run",
                                  "Installing the build tools needs administrator rights: accept the Windows prompt."}},
            {"install_failed", {"The installer didn't finish",
                                "Run it again, or install \"Build Tools for Visual Studio\" yourself from "
                                "visualstudio.microsoft.com with \"Desktop development with C++\" and \"C++ Clang tools "
                                "for Windows\"."}},
            {"reboot_needed", {"Restart Windows to finish", "The installer needs a restart before the tools work."}},
            {"python_failed", {"A tool could not run", "Python stopped with an error: open the log."}},
            {"git_checkout", {"This is a git checkout", "Update it with git pull, then build again."}},
            {"build_dir_refused", {"That folder can't be used", "Choose another folder for the build."}},
            {"network", {"No connection", "Check the internet connection and try again."}},
            {"update_incomplete", {"The update couldn't replace every file",
                                   "Something held some of them open (often an antivirus or a cloud-synced folder); "
                                   "the folder is still the old release. Close the game and update again, or extract "
                                   "the new release's zip into a new folder."}},
        };
#ifndef _WIN32
        // the same codes where Linux needs other words, and its own (tools/build.py --install-deps, doctor.py)
        const std::map<std::string, ErrorHelp> kLinuxErrors = {
            {"missing_packages", {"Packages the build needs are missing",
                                  "Use \"Install packages\" on the Build tools step (it asks for your password), or "
                                  "\"Install in a terminal\"."}},
            {"no_polkit", {"No password prompt could be shown",
                           "Use \"Install in a terminal\" on the Build tools step: the command runs there and asks "
                           "for your password."}},
            {"elevation_denied", {"The password prompt was cancelled",
                                  "Installing packages needs your password: use \"Install packages\" again and enter it."}},
            {"install_failed", {"The package manager didn't finish",
                                "Open the log for its message. Another update running at the same time locks it: try "
                                "again once that is done, or use \"Install in a terminal\"."}},
            {"file_locked", {"A file was locked", "Something else held a file of the build. Build again."}},
            {"file_dates", {"Files are dated in the future",
                            "Some files in the folder are newer than the PC's clock, so the build keeps starting over. "
                            "Check the clock and time zone, then build again."}},
            {"out_of_memory", {"The PC ran out of memory",
                               "Close other programs and build again; it continues where it stopped."}},
        };
#endif
        const ErrorHelp kUnknown = {"Something went wrong", "Open the log for the details, or copy a report for help."};
    }

    const ErrorHelp &errorHelp(const std::string &code)
    {
#ifndef _WIN32
        if (const auto it = kLinuxErrors.find(code); it != kLinuxErrors.end())
            return it->second;
#endif
        const auto it = kErrors.find(code);
        return it == kErrors.end() ? kUnknown : it->second;
    }

    bool App::init(std::string &error)
    {
#ifndef _WIN32
        // a Linux AppImage: the release's files are in the image next to the launcher (usr/bin -> usr/share), and the
        // game is built in a folder of its own they are copied to
        if (const fs::path files = exePath().parent_path().parent_path() / "share" / "vcs-recomp" / "release";
            fs::is_regular_file(files / "RELEASE.json"))
        {
            payload = files;
            const char *dir = std::getenv("VCS_INSTALL_DIR");
            root = dir && *dir ? pathFrom(dir) : knownFolder(Folder::LocalAppData) / "VCSRecomp" / "install";
        }
#endif
        // the repository: the exe's folder (a release, or a clone after build.py --launcher), or one above it
        for (fs::path dir = exePath().parent_path(); root.empty() && !dir.empty(); dir = dir.parent_path())
        {
            if (fs::is_regular_file(dir / "tools" / "build.py"))
            {
                root = dir;
                break;
            }
            if (dir == dir.parent_path())
                break;
        }
        if (root.empty())
        {
            error = std::string(os::kLauncherExe) + " must stay in the game's folder, next to tools and src (it builds the "
                    "game from there). It was started from " + pathText(exePath().parent_path());
            return false;
        }
        // the build folder as the tools see it (toolchain.BUILD): VCS_BUILD_DIR, else .vcs-build-dir, else build/
        build = root / "build";
        if (const char *env = std::getenv("VCS_BUILD_DIR"); env && *env)
            build = pathFrom(env);
        else if (const std::string chosen = trim(readFile(root / ".vcs-build-dir")); !chosen.empty())
            build = pathFrom(chosen);
        {
            // an update replaces the running launcher by renaming it (tools/update.py set_aside): gone now
            std::error_code ec;
            fs::remove(root / (std::string(os::kLauncherExe) + ".old"), ec);
        }

        // a release's own python\ first; else the PC's, each tried (a py.exe can exist with no Python behind it, and
        // the Microsoft Store's python.exe stub only opens the Store). Linux: the distro's python3
        std::vector<std::vector<std::string>> candidates;
#ifdef _WIN32
        if (fs::path bundled = root / "python" / "python.exe"; fs::is_regular_file(bundled))
            candidates.push_back({pathText(bundled)});
        if (std::string exe = findOnPath("python.exe"); !exe.empty() && exe.find("WindowsApps") == std::string::npos)
            candidates.push_back({exe});
        if (std::string py = findOnPath("py.exe"); !py.empty())
            candidates.push_back({py, "-3"});
#else
        if (fs::path bundled = root / "python" / "bin" / "python3"; fs::is_regular_file(bundled))
            candidates.push_back({pathText(bundled)});
        for (const char *name : {"python3", "python"})
            if (std::string exe = findOnPath(name); !exe.empty())
                candidates.push_back({exe});
#endif
        for (const auto &candidate : candidates)
        {
            std::vector<std::string> argv = candidate;
            argv.insert(argv.end(), {"-I", "-c", "import sys; print(sys.version_info >= (3, 8))"});
            Process probe;
            Process::Options options;
            std::string ignored;
            if (!probe.start(argv, options, ignored))
                continue;
            probe.wait();
            const auto lines = probe.takeLines();
            if (probe.exitCode() == 0 && !lines.empty() && lines.back() == "True")
            {
                python = candidate;
                break;
            }
        }
        if (python.empty())
        {
            error = os::kWindows
                        ? "Python 3 wasn't found. A release brings its own (the python folder next to the launcher); in a "
                          "git clone, install Python from python.org (tick \"Add python.exe to PATH\")."
                        : "Python 3 wasn't found: install your distribution's python3 package (Python 3.8 or newer).";
            return false;
        }
        pythonText = python[0];

        // an AppImage whose files aren't in the install folder yet, or are another version's: they go in place first
        // (tools/update.py install, the image's own copy; a task the window shows), then the folder opens
        if (!payload.empty() && readFile(payload / "RELEASE.json") != readFile(root / "RELEASE.json"))
        {
            std::error_code ec;
            fs::create_directories(root, ec);
            if (ec)
            {
                error = "Could not create the game's folder " + pathText(root) + " (" + ec.message() + ")";
                return false;
            }
            sourceVersion = trim(readFile(payload / "VERSION")); // the title bar's, before the folder opens
            runTool("install", "Putting the game's files in place",
                    {toolPath(payload, "update.py"), "install", "--from", pathText(payload), "--to", pathText(root)});
            return true;
        }
        loadFolder();
        return true;
    }

    void App::loadFolder()
    {
        // the scripts every button runs: all there, or the folder doesn't match this launcher - a release made from the
        // last commit while the launcher was built from newer, uncommitted code, or files lost on the way
        {
            std::string missing;
            int count = 0;
            for (const char *tool : kTools)
                if (!fs::is_regular_file(root / "tools" / tool))
                {
                    missing += std::string(count ? ", " : "") + tool;
                    ++count;
                }
            if (count)
                toolsProblem = "The launcher's scripts are missing from " + std::string(os::kTools) + " (" + missing +
                               "), so nothing can be checked. Download the release again. If you made this release "
                               "yourself, its scripts are older than the launcher: tools/release.py packages the last "
                               "commit - commit first, or make it with --worktree.";
        }

        // VERSION (the repository's, or the one a release wrote); a clone adds its commit, since the same number
        // covers many commits there
        sourceVersion = trim(readFile(root / "VERSION"));
        {
            std::string head = trim(readFile(root / ".git" / "HEAD"));
            if (head.rfind("ref:", 0) == 0)
                head = trim(readFile(root / ".git" / trim(head.substr(4))));
            if (head.size() >= 7)
                sourceVersion = sourceVersion.empty() ? "git " + head.substr(0, 7) : sourceVersion + " (" + head.substr(0, 7) + ")";
            else if (sourceVersion.empty())
                sourceVersion = "development";
        }
        loadSettings();

        // the disc vcs.exe would use
        std::string iniIso;
        {
            std::ifstream in(build / "vcs.ini");
            std::string line;
            while (std::getline(in, line))
                if (line.rfind("iso=", 0) == 0)
                    iniIso = trim(line.substr(4));
        }
        if (!iniIso.empty() && iniIso.front() == '"')
            iniIso = iniIso.substr(1, iniIso.size() - 2);
        if (!iniIso.empty())
        {
            // a relative iso= (written by hand) is the build folder's, or the source folder's, not the launcher's cwd
            fs::path isoPath = pathFrom(iniIso);
            std::error_code ec;
            if (isoPath.is_relative())
                for (const fs::path &base : {build, root})
                    if (fs::is_regular_file(base / isoPath, ec))
                    {
                        isoPath = base / isoPath;
                        break;
                    }
            chooseDisc(isoPath);
        }
        if (!disc.ok)
            scanForDisc();
        {
            bool ok = false;
            Json cached = Json::parse(readFile(build / "launcher_state.json"), &ok);
            if (ok && cached.isObject() && !cached["next"].str().empty())
            {
                doctor = cached;
                doctorDone = doctorCached = true;
            }
        }
        refresh();
    }

    bool App::canUpdate() const
    {
#ifdef _WIN32
        return true;
#else
        return !payload.empty();
#endif
    }

    void App::refresh()
    {
        if (m_doctor)
            return;
        if (!toolsProblem.empty()) // the scripts aren't there: nothing to run, say why
        {
            doctor = Json();
            doctorError = toolsProblem;
            doctorDone = true;
            return;
        }
        std::vector<std::string> argv = python;
        argv.insert(argv.end(), {"-I", toolPath(root, "doctor.py"), "--json"});
        Process::Options options;
        options.cwd = root;
        options.env = {{"PYTHONUTF8", "1"}, {"PYTHONIOENCODING", "utf-8"}};
        m_doctor = std::make_unique<Process>();
        m_doctorOut.clear();
        std::string error;
        if (!m_doctor->start(argv, options, error))
        {
            doctorError = error;
            m_doctor.reset();
            return;
        }
        doctorRunning = true;
    }

    void App::say(const std::string &text, double now)
    {
        toast = text;
        toastTime = now < 0 ? m_now : now;
    }

    void App::update(double now)
    {
        m_now = now;
        if (m_doctor)
        {
            for (auto &line : m_doctor->takeLines())
                m_doctorOut.push_back(std::move(line));
            if (!m_doctor->running())
            {
                for (auto &line : m_doctor->takeLines())
                    m_doctorOut.push_back(std::move(line));
                bool ok = false;
                for (auto it = m_doctorOut.rbegin(); it != m_doctorOut.rend() && !ok; ++it)
                    if (!it->empty() && (*it)[0] == '{')
                    {
                        Json parsed = Json::parse(*it, &ok);
                        if (ok)
                        {
                            doctor = parsed;
                            std::error_code ec;
                            fs::create_directories(build, ec);
                            std::ofstream(build / "launcher_state.json", std::ios::binary | std::ios::trunc) << *it << '\n';
                        }
                    }
                doctorError.clear();
                if (!ok)
                {
                    doctor = Json(); // no answer: nothing below may read an old or empty one as "missing"
                    doctorError = "tools/doctor.py failed";
                    for (size_t i = m_doctorOut.size() > 12 ? m_doctorOut.size() - 12 : 0; i < m_doctorOut.size(); ++i)
                        doctorError += "\n" + m_doctorOut[i];
                }
                doctorRunning = false;
                doctorDone = true;
                doctorCached = false;
                m_doctor.reset();
            }
        }

        if (task && task->process)
        {
            for (const auto &line : task->process->takeLines())
                handleLine(*task, line);
            if (!task->process->running())
            {
                for (const auto &line : task->process->takeLines())
                    handleLine(*task, line);
                task->exitCode = task->process->exitCode();
                task->process.reset();
                finishTask(*task);
            }
        }

        if (game && gameRunning && !game->running())
        {
            gameExit = game->exitCode();
            gameRunning = false;
            gameEnded = true;
            gameEnd = now;
            game.reset();
        }
    }

    void App::handleLine(Task &t, const std::string &line)
    {
        if (line.rfind(kEventPrefix, 0) == 0)
        {
            bool ok = false;
            const Json e = Json::parse(std::string_view(line).substr(5), &ok);
            if (!ok)
                return;
            if (e.has("stage"))
            {
                if (!t.stages.empty() && t.stages.back().end == 0)
                    t.stages.back().end = m_now;
                t.stages.push_back({e["stage"].str(), e["title"].str(), m_now, 0});
                t.done = t.total = 0;
                t.eta = -1;
                t.note.clear();
            }
            if (e.has("progress"))
            {
                t.done = int(e["progress"].num());
                t.total = int(e["total"].num());
                t.eta = e["eta"].type() == Json::Type::Number ? int(e["eta"].num()) : -1;
                t.note = e["note"].str();
            }
            if (e.has("error") && t.errorCode.empty())
            {
                t.errorCode = e["error"].str();
                t.errorMessage = e["message"].str();
                t.errorLog = e["log"].str();
            }
            if (e.has("result"))
                t.result = e;
            return;
        }
        t.log.push_back(line);
        if (t.log.size() > kLogLines)
            t.log.pop_front();
    }

    void App::finishTask(Task &t)
    {
        t.finished = true;
        t.endTime = m_now;
        if (!t.stages.empty() && t.stages.back().end == 0)
            t.stages.back().end = m_now;
        keepAwake(false);
        if (!t.cancelled && t.exitCode != 0 && t.errorCode.empty())
        {
            t.errorCode = t.kind == "install" ? "install_failed" : "python_failed";
            t.errorMessage = t.title + " stopped (exit code " + std::to_string(t.exitCode) + ")";
        }
        if (t.kind == "install" && t.ok())
        {
            task.reset(); // the files are in place: the folder opens as if they had always been there
            loadFolder();
            return;
        }
        if (t.kind == "build_dir_check" && t.result.has("path"))
        {
            buildDirCheck = t.result; // "can't use it" is an answer too (exit 1): the confirmation shows why
            task.reset();
            return;
        }
        if (t.kind == "build_dir_set" && t.ok())
        {
            build = pathFrom(t.result["path"].str());
            loadSettings(); // launcher.ini came along
            say(t.result["old_built"].boolean()
                    ? "Build folder changed. The game is built again there; the old folder can be deleted afterwards: " +
                          t.result["previous"].str()
                    : "Build folder changed: " + t.result["path"].str());
            task.reset();
            refresh();
            return;
        }
        if (t.kind == "backup_list" && t.ok())
            backups = t.result["backups"];
        if (t.kind == "rip_list" && t.ok())
            drives = t.result["drives"];
        if ((t.kind == "update_check") && t.ok())
            updateInfo = t.result;
        if (t.kind == "update" && t.ok() && t.result["restart"].boolean())
        {
            updatedTo = t.result["version"].str(); // in place: this launcher is the old one until it restarts
            updateInfo = Json();
        }
        if (t.kind == "rip" && t.ok() && t.result.has("iso"))
            chooseDisc(pathFrom(t.result["iso"].str()));
        if (t.kind == "backup" && t.ok())
            say("Saves backed up: " + t.result["path"].str());
        if (t.kind == "restore" && t.ok())
            say("Saves restored (the card before it was backed up too)");
        if (t.kind == "update_check" && t.ok() && !updateQuiet && !t.result["newer"].boolean())
            say(t.result["message"].str().empty() ? "You have the latest release." : t.result["message"].str());
        // a quiet check that found nothing new, or failed, leaves no trace
        if (t.kind == "update_check" && updateQuiet && (!t.ok() || !t.result["newer"].boolean()))
            task.reset();
        else if ((t.kind == "backup_list" || t.kind == "rip_list" || t.kind == "update_check" || t.kind == "backup" ||
                  t.kind == "restore") && t.ok())
            task.reset(); // their results show elsewhere
        refresh();
    }

    void App::runTool(const std::string &kind, const std::string &title, const std::vector<std::string> &args)
    {
        if (busy())
            return;
        task = std::make_unique<Task>();
        task->kind = kind;
        task->title = title;
        task->startTime = m_now;
        std::vector<std::string> argv = python;
        argv.push_back("-I");
        argv.insert(argv.end(), args.begin(), args.end());
        Process::Options options;
        options.cwd = root;
        options.env = {{"VCS_PROGRESS", "json"}, {"PYTHONUTF8", "1"}, {"PYTHONIOENCODING", "utf-8"},
                       {"PYTHONUNBUFFERED", "1"}};
        task->process = std::make_unique<Process>();
        std::string error;
        if (!task->process->start(argv, options, error))
        {
            task->process.reset();
            task->errorCode = "python_failed";
            task->errorMessage = error;
            finishTask(*task);
        }
    }

    void App::startBuild()
    {
        std::vector<std::string> args = {toolPath(root, "build.py"), "--recompile-if-needed", pathText(iso), "--progress",
                                         "json"};
        if (!settings.opt.empty())
            args.insert(args.end(), {"--opt", settings.opt});
        runTool("build", "Building the game", args);
        if (busy())
            keepAwake(true);
    }

    void App::startInstallTools()
    {
#ifdef _WIN32
        runTool("tools", "Installing the build tools", {toolPath(root, "setup_vs.py"), "install"});
#else
        // the distribution's packages, through pkexec (the desktop's password prompt): build.py --install-deps
        runTool("tools", "Installing the packages the build needs",
                {toolPath(root, "build.py"), "--install-deps", "--progress", "json"});
#endif
    }

    void App::startBackup() { runTool("backup", "Backing up your saves", {toolPath(root, "backup.py"), "create"}); }

    void App::startBackupList() { runTool("backup_list", "Listing backups", {toolPath(root, "backup.py"), "list"}); }

    void App::startRestore(const std::string &name)
    {
        runTool("restore", "Restoring saves", {toolPath(root, "backup.py"), "restore", name});
    }

    void App::startReport() { runTool("report", "Making a report", {toolPath(root, "report.py")}); }

    void App::startUpdateCheck(bool quiet)
    {
        updateQuiet = quiet;
        runTool("update_check", "Checking for updates", {toolPath(root, "update.py"), "check"});
    }

    void App::startUpdate() { runTool("update", "Updating", {toolPath(root, "update.py"), "apply"}); }

    void App::startBuildDirCheck(const fs::path &path)
    {
        buildDirCheck = Json();
        runTool("build_dir_check", "Checking the folder", {toolPath(root, "build_dir.py"), "check", pathText(path)});
    }

    void App::startBuildDirSet(const fs::path &path)
    {
        runTool("build_dir_set", "Changing the build folder", {toolPath(root, "build_dir.py"), "set", pathText(path)});
    }

    bool App::buildCustom() const
    {
        std::error_code ec;
        return !fs::equivalent(build, root / "build", ec);
    }

    void App::startRipList() { runTool("rip_list", "Looking for discs", {toolPath(root, "rip_disc.py"), "list"}); }

    void App::startRip(const std::string &drive, const fs::path &out)
    {
        runTool("rip", "Copying the disc", {toolPath(root, "rip_disc.py"), "rip", drive, pathText(out)});
        if (busy())
            keepAwake(true);
    }

    void App::cancelTask()
    {
        if (!busy() || !task->process)
            return;
        task->cancelled = true;
        task->process->kill();
    }

    void App::chooseDisc(const fs::path &path, bool byScan)
    {
        DiscResult result = checkDisc(path);
        if (byScan && !result.ok)
            return;
        iso = path;
        disc = result;
        discChecked = true;
        discFound = byScan;
        if (disc.ok && !writeIsoToIni(path))
            say(std::string("Could not write ") + os::kBuildIni + " - is the folder read-only?");
        if (!byScan)
            refresh();
    }

    void App::scanForDisc()
    {
        for (const fs::path &candidate : discCandidates(root))
        {
            chooseDisc(candidate, true);
            if (disc.ok)
                return;
        }
    }

    bool App::writeIsoToIni(const fs::path &path)
    {
        // vcs.exe's settings keep every line they don't own (src/settings/settings.cpp), iso= among them
        const fs::path ini = build / "vcs.ini";
        std::vector<std::string> lines;
        {
            std::ifstream in(ini);
            std::string line;
            while (std::getline(in, line))
                if (line.rfind("iso=", 0) != 0)
                    lines.push_back(line);
        }
        lines.push_back("iso=" + pathText(path));
        std::error_code ec;
        fs::create_directories(build, ec);
        std::ofstream out(ini, std::ios::trunc | std::ios::binary);
        for (const auto &l : lines)
            out << (l.empty() || l.back() != '\r' ? l : l.substr(0, l.size() - 1)) << "\n";
        return bool(out);
    }

    bool App::play(std::string &error)
    {
        if (gameRunning)
            return true;
        const fs::path exe = build / os::kGameExe;
        if (!fs::is_regular_file(exe))
        {
            error = std::string(os::kBuildGame) + " isn't there: build the game first";
            return false;
        }
        gameLog = build / "run" / "launcher_game.log";
        Process::Options options;
        options.cwd = build;
        options.outputFile = gameLog;
        options.killOnClose = false; // the game plays on when the launcher closes
        game = std::make_unique<Process>();
        if (!game->start({pathText(exe), pathText(iso)}, options, error))
        {
            game.reset();
            return false;
        }
        gameRunning = true;
        gameEnded = false;
        gameStart = m_now;
        return true;
    }

    fs::path App::restartTarget() const
    {
        // Linux: the AppImage file (update.py renamed the new one over it; the old image stays mounted until we end)
        if (const char *appimage = std::getenv("APPIMAGE"); appimage && *appimage && !payload.empty())
            return pathFrom(appimage);
        // Windows: the release's launcher, written fresh by update.py (this one runs as VCSLauncher.exe.old)
        return root / os::kLauncherExe;
    }

    bool App::restart(std::string &error)
    {
        const fs::path exe = restartTarget();
        if (!fs::is_regular_file(exe))
        {
            error = "The new launcher isn't there: " + pathText(exe);
            return false;
        }
        Process::Options options;
        options.cwd = root;
        // its output to a file, not a pipe: the pipe's reader (us) is about to end
        options.outputFile = build / "run" / "launcher_restart.log";
        options.killOnClose = false; // it outlives us
        m_restarted = std::make_unique<Process>();
        if (!m_restarted->start({pathText(exe), "--after-update", std::to_string(processId())}, options, error))
        {
            m_restarted.reset();
            return false;
        }
        return true;
    }

    std::string App::gameExitText() const
    {
        const unsigned code = static_cast<unsigned>(gameExit);
        switch (gameExit)
        {
        case 0: return "";
        case 2: return "The game found no disc image. Choose your disc again.";
        case 3: return "The game could not read the disc image (moved, or on a drive that's gone?).";
        case 4: return "The game says this is not the supported disc (GTA: Vice City Stories USA, SLUS-21590).";
        case 5: return "The game could not start its window, graphics or sound. Update the graphics driver; "
                       "the log has the details.";
        default: break;
        }
#ifndef _WIN32
        // a death by signal N is 128 + N (model/process_posix.cpp)
        if (gameExit > 128 && gameExit < 128 + 32)
        {
            const int sig = gameExit - 128;
            const char *what = sig == 11 ? "segmentation fault" : sig == 4 ? "illegal instruction - built for a newer processor?"
                             : sig == 6  ? "aborted" : sig == 7 ? "bus error" : sig == 8 ? "arithmetic error"
                             : sig == 9  ? "killed - out of memory?" : nullptr;
            if (sig == 15 || sig == 2 || sig == 1)
                return "The game was stopped from outside (signal " + std::to_string(sig) + ").";
            const std::string name = what ? std::string(what) : "signal " + std::to_string(sig);
            return "The game crashed (" + name + "). The log has the details: please copy a report.";
        }
#endif
        if (code == 0xC0000005u)
            return "The game crashed (access violation). The log has the details: please copy a report.";
        if (code >= 0xC0000000u)
        {
            char hex[16];
            std::snprintf(hex, sizeof(hex), "0x%08X", code);
            return std::string("The game crashed (") + hex + "). The log has the details: please copy a report.";
        }
        return "The game stopped with an error (code " + std::to_string(gameExit) + "). The log has the details.";
    }

    bool App::shortcut(std::string &error)
    {
        const fs::path desktop = knownFolder(Folder::Desktop);
        if (desktop.empty())
        {
            error = "no Desktop folder";
            return false;
        }
        // an AppImage starts from its own file, not from where it is mounted this time
        fs::path target = exePath();
        if (const char *appimage = std::getenv("APPIMAGE"); appimage && *appimage && !payload.empty())
            target = pathFrom(appimage);
        return createShortcut(desktop / os::kShortcutName, target, root,
                              "GTA: Vice City Stories (PC port) - launcher", error);
    }

    fs::path App::savesFolder() const
    {
        if (const char *dir = std::getenv("VCS_DATA_DIR"); dir && *dir)
            return pathFrom(dir);
        return knownFolder(Folder::LocalAppData) / "VCSRecomp";
    }

    std::string App::next() const
    {
        if (!disc.ok)
            return "disc";
        if (doctorFailed())
            return "recheck";
        const std::string n = doctor["next"].str();
        if (n == "disc") // the doctor ran before the disc was chosen
            return doctorDone ? "build" : "";
        return n;
    }

    void App::loadSettings()
    {
        std::ifstream in(build / "launcher.ini");
        std::string line;
        while (std::getline(in, line))
        {
            line = trim(line);
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
            if (key == "after_launch" && (value == "minimize" || value == "close" || value == "stay"))
                settings.afterLaunch = value;
            else if (key == "opt" && (value.empty() || value == "lto" || value == "none" || value == "pgo"))
                settings.opt = value;
            else if (key == "check_updates")
                settings.checkUpdates = value == "1";
            else if (key == "theme" && (value == "system" || value == "light" || value == "dark"))
                settings.theme = value;
            else if (key == "reduce_motion")
                settings.reduceMotion = value == "1";
        }
    }

    void App::saveSettings()
    {
        std::error_code ec;
        fs::create_directories(build, ec);
        std::ofstream out(build / "launcher.ini", std::ios::trunc | std::ios::binary);
        out << "# the launcher's own settings (the game's are in vcs.ini)\n"
            << "after_launch=" << settings.afterLaunch << "\n"
            << "opt=" << settings.opt << "\n"
            << "check_updates=" << (settings.checkUpdates ? 1 : 0) << "\n"
            << "theme=" << settings.theme << "\n"
            << "reduce_motion=" << (settings.reduceMotion ? 1 : 0) << "\n";
    }
}
