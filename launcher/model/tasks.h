// The launcher's state and what it runs. Everything long is a Python tool from tools/ (the same scripts people run by
// hand): doctor.py for the checks, build.py for the build, setup_vs.py, backup.py, report.py, update.py, rip_disc.py.
// They print `@vcs {...}` events (tools/toolchain.py) that become a Task's stages, progress, error and result.
#pragma once

#include "disc_check.h"
#include "json.h"
#include "process.h"

#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace launcher
{
    struct Task
    {
        std::string kind;  // build, tools, backup, restore, report, update_check, update, rip_list, rip
        std::string title;
        std::unique_ptr<Process> process;
        struct Stage
        {
            std::string id, title;
            double start = 0, end = 0;
        };
        std::vector<Stage> stages;
        int done = 0, total = 0, eta = -1;
        std::string note;
        std::deque<std::string> log;
        std::string errorCode, errorMessage, errorLog;
        Json result;
        bool finished = false, cancelled = false;
        int exitCode = 0;
        double startTime = 0, endTime = 0;

        bool ok() const { return finished && !cancelled && exitCode == 0 && errorCode.empty(); }
    };

    // What a failure code from the tools means and what the player can do (shown under the error).
    struct ErrorHelp
    {
        const char *title;
        const char *fix;
    };
    const ErrorHelp &errorHelp(const std::string &code);

    struct Settings
    {
        std::string afterLaunch = "minimize"; // minimize, close, stay
        std::string opt;                      // "" (keep the build's), lto, none, pgo
        bool checkUpdates = false;            // ask GitHub for a newer release when the launcher starts
        bool reduceMotion = false;            // the launcher's animations off; this alone decides (off until set)
        std::string theme = "system";         // system (Windows' app mode), light, dark
    };

    class App
    {
    public:
        bool init(std::string &error);
        void update(double now); // polls the processes; call every frame

        // the repository (where tools/build.py is) and its build folder
        std::filesystem::path root, build;
        // Linux AppImage (tools/release.py): the release's files inside the read-only image; root is then the install
        // folder they are copied to ($XDG_DATA_HOME/VCSRecomp/install, VCS_INSTALL_DIR) - empty otherwise
        std::filesystem::path payload;
        bool installing() const { return task && task->kind == "install" && !task->finished; }
        // a release that updates itself: Windows (a release folder or not: update.py says), a Linux AppImage
        bool canUpdate() const;
        std::vector<std::string> python; // the interpreter (+ args): a release's python\, else py -3 / python (Linux: python3)
        std::string pythonText;
        std::string sourceVersion;        // VERSION, or the clone's commit

        // tools/doctor.py --json
        Json doctor;
        bool doctorRunning = false, doctorDone = false;
        // shown from build/launcher_state.json (the last answer) until this run's check answers: the first frame
        // already has the right layout; nothing in it may act on what the cache says (the UI shows it as checking)
        bool doctorCached = false;
        std::string doctorError;
        std::string toolsProblem; // the scripts the launcher runs aren't all in tools/ (set by init)
        // the check ran and gave no answer: the steps it decides are unknown, never "missing"
        bool doctorFailed() const { return doctorDone && !doctorRunning && !doctor.isObject(); }
        void refresh();

        // the disc: checked here (instantly), remembered as iso= in build/vcs.ini
        std::filesystem::path iso;
        DiscResult disc;
        bool discChecked = false, discFound = false; // found = picked by the scan, not the player
        void chooseDisc(const std::filesystem::path &path, bool byScan = false);
        void scanForDisc();

        std::unique_ptr<Task> task; // the one long thing running (or finished, until dismissed)
        bool busy() const { return task && !task->finished; }
        void startBuild();
        void startInstallTools();
        void startBackup();
        void startRestore(const std::string &backup);
        void startBackupList();
        void startReport();
        void startUpdateCheck(bool quiet);
        void startUpdate();
        void startRipList();
        // the build folder (tools/build_dir.py): check what a chosen folder means, then use it
        void startBuildDirCheck(const std::filesystem::path &path);
        void startBuildDirSet(const std::filesystem::path &path);
        Json buildDirCheck; // the last check's answer, for the confirmation
        bool buildCustom() const;
        void startRip(const std::string &drive, const std::filesystem::path &out);
        void cancelTask();
        void dismissTask() { if (task && task->finished) task.reset(); }
        Json backups, drives, updateInfo; // results kept from their tasks
        bool updateQuiet = false;
        // an update put in place (update.py apply said restart): the version; this launcher is the old one until then
        std::string updatedTo;
        std::filesystem::path restartTarget() const; // the new launcher: root's VCSLauncher.exe, or $APPIMAGE
        // the new launcher started (--after-update <our pid>: it waits for us to end); the caller closes this one
        bool restart(std::string &error);

        // the game
        std::unique_ptr<Process> game;
        bool gameRunning = false, gameEnded = false;
        int gameExit = 0;
        double gameStart = 0, gameEnd = 0;
        std::filesystem::path gameLog;
        bool play(std::string &error);
        std::string gameExitText() const;

        Settings settings;
        void saveSettings();

        std::string toast; // a short message under the buttons
        double toastTime = 0;
        void say(const std::string &text, double now = -1);

        bool shortcut(std::string &error);
        std::filesystem::path savesFolder() const;
        std::string next() const; // doctor's "next" with the launcher's own disc check over it; "recheck" when it failed

    private:
        std::unique_ptr<Process> m_doctor;
        std::unique_ptr<Process> m_restarted; // the new launcher (restart): never killed
        std::vector<std::string> m_doctorOut;
        double m_now = 0;
        void runTool(const std::string &kind, const std::string &title, const std::vector<std::string> &args);
        void handleLine(Task &t, const std::string &line);
        void finishTask(Task &t);
        void loadSettings();
        void loadFolder(); // root's scripts, version, settings, disc, the doctor - once root holds the files
        bool writeIsoToIni(const std::filesystem::path &path);
    };
}
