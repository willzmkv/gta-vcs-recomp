// What the screens share: the window's UI state (mode, setup step, open sheet and dialog, toasts) and the readings of
// the model they all use. Internal to screens/.
#pragma once

#include "model/tasks.h"
#include "render/draw.h"
#include "render/font.h"
#include "screens/screens.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#include <functional>
#include <string>
#include <vector>

namespace screens
{
    using core::Color;
    using core::Rect;
    using launcher::App;
    using ui::px;

    enum class Mode
    {
        Setup,
        Home,
        Task,
    };

    enum class Sheet
    {
        None,
        Saves,
        Settings,
        Help,
        Folder,
    };

    enum class Dialog
    {
        None,
        Restore,
        Quit,
        CancelTask,
        BuildDir,
    };

    // input layers (ui::beginLayer)
    constexpr int kLayerSheet = 2, kLayerDialog = 3;

    enum Step
    {
        kStepFolder,
        kStepDisc,
        kStepTools,
        kStepGame,
        kStepCount
    };

    struct Toast
    {
        std::string text;
        double since = 0;
        Color color = 0;
    };

    struct Shell
    {
        App *app = nullptr;
        Options options;

        Mode mode = Mode::Home, previousMode = Mode::Home;
        double modeSince = -10;  // animation clock
        bool setupOpen = false;  // the player opened Setup from Home (everything is ready)

        int step = kStepDisc, previousStep = -1;
        double stepSince = -10;
        int pinnedStep = -1;     // picked on the rail; -1: follow what's next
        double stepGoodSince = -1; // the shown step turned good: hold a moment before moving on
        bool folderSeen = false; // the folder step with warnings was looked at

        Sheet sheet = Sheet::None, sheetShown = Sheet::None;
        core::Spring sheetSpring;

        Dialog dialog = Dialog::None, dialogShown = Dialog::None;
        float dialogT = 0;

        std::string restoreName, ripDrive;
        bool showDrives = false;
        bool showLog = false;
        bool backupsAsked = false;
        bool awaitingBuildDir = false;
        bool minimizedForGame = false;
        bool quitNow = false;
        bool gameErrorDismissed = false;

        std::vector<Toast> toasts;
        std::string lastToast;
        double lastToastTime = -1;

        std::function<void()> pending; // a file dialog, run between frames
    };
    extern Shell shell;

    // ---- readings of the model (screens.cpp) -----------------------------------------------------------------------
    ui::Status folderStatus(const App &app);
    ui::Status discStatus(const App &app);
    ui::Status toolsStatus(const App &app);
    ui::Status gameStatus(const App &app);
    ui::Status stepStatus(const App &app, int step);
    std::string folderLine(const App &app);
    std::string discLine(const App &app);
    std::string toolsLine(const App &app);
    std::string gameLine(const App &app);
    std::string stepLine(const App &app, int step);
    const char *stepName(int step);
    bool cpuUnsupported(const App &app);
    bool takesOver(const launcher::Task &t); // a task that takes the window (Task mode)
    std::string duration(double seconds);
    std::string gigabytes(double bytes);
    std::string buildSummary(const App &app); // "Standard · built 2 days ago"
    bool canPlay(const App &app);
    bool reduceMotion(const App &app); // the launcher's setting (off until set)
    bool darkTheme(const App &app);    // the player's choice, else Windows' app mode

    // ---- actions shared by several screens -------------------------------------------------------------------------
    void chooseIso();                        // the file dialog
    void chooseBuildDir(const std::filesystem::path &start);
    void chooseRipTarget();
    void play(App &app);
    void restartLauncher(App &app); // after an update: the new launcher starts, this one closes
    void openSheet(Sheet sheet);
    void openDialog(Dialog dialog);
    void openSetup(int step);
    void toast(const std::string &text, Color color = 0);

    // ---- layout helpers --------------------------------------------------------------------------------------------
    // a group appearing: index i of a staggered entrance that began at `since` (animation clock); 0..1
    float reveal(double since, int index);
    // pushes the entrance's opacity and slide; pop with popReveal()
    void pushReveal(double since, int index, float distance = 14);
    void popReveal();
    // a heading block: eyebrow, title and description from r.y; returns the y under it
    float header(Rect r, std::string_view eyebrow, Color eyebrowColor, std::string_view title, std::string_view description);
    // a label / value row; returns its height
    float row(Rect r, std::string_view name, std::string_view value, Color valueColor = 0);
    float windowScale();

    // ---- the parts (each in its own file) --------------------------------------------------------------------------
    void drawBackdrop(Rect r, float hero, bool alive = false); // hero: 0..1, the scene of Home; alive: it moves
    float drawTitleBar(Rect r);              // returns its height
    void drawSetup(Rect r);
    void drawHome(Rect r);
    void drawTask(Rect r);
    void drawSheets(Rect window);
    void drawDialogs(Rect window);
    void drawToasts(Rect window);
    void drawGallery(Rect r);
    void drawFolderBody(Rect r, float &y);   // the folder step's content, also the Folder sheet's
}
