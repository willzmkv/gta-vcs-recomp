// Player settings (docs/SETTINGS_MENU_WIDESCREEN.md): a registry of typed settings, their values (vcs.ini next to
// vcs.exe), and change notifications. The settings menu (ui/settings_menu.h) is generated from the registry: a new
// setting is one add() plus its apply callback.
//
// Order at start-up: add() everything, load(ini), applyAll(). Values are ints (toggles, choices, integers, keys;
// actions have none).
// Threads: add / load / applyAll / set from the main thread (menu); value() from any thread.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace settings
{
    enum class Type
    {
        Toggle,  // 0 / 1
        Choice,  // one of `choices` (their values)
        Integer, // min..max
        Key,     // a platform::Key, mouse buttons and wheel included (the menu offers the keys it knows)
        Action,  // a button: onChange(0) when pressed (presets, reset); no value, nothing in vcs.ini
    };

    enum class Apply
    {
        Live,    // onChange runs at once
        Restart, // takes effect at the next start
    };

    struct Choice
    {
        std::string label;
        int value = 0;
    };

    struct Spec
    {
        std::string key;     // vcs.ini key, e.g. "display.aspect"
        std::string alias;   // an older vcs.ini key, read when `key` is absent (e.g. "scale")
        std::string page;    // menu page (a tab), e.g. "Display"; independent of `key`, so settings can move pages
        std::string section; // heading inside the page, e.g. "Screen" (rows of one section are registered together)
        std::string label;   // "Aspect ratio"
        std::string help;    // one line, shown for the selected setting
        Type type = Type::Toggle;
        int defaultValue = 0;
        int min = 0, max = 1;          // Integer
        std::string format = "%d";     // Integer, as shown
        std::vector<Choice> choices;   // Choice
        Apply apply = Apply::Live;
        std::string env;               // an environment variable that overrides it (testing), shown as such
        std::function<void(int)> onChange; // the new value; also called once by applyAll()
        std::function<int()> read;     // optional: the live value, when something else changes it too (a hotkey)
        // false: not in vcs.ini - the value lives elsewhere (a mod's priority in modloader.ini): `read` gives it at load
        // and refresh, onChange stores it
        bool saved = true;
        // Optional: another setting decides this one for now (e.g. the cutscene bars at 4:3). The menu greys the row
        // out, shows `lockedValue` and adds `lockedNote` to its help; the saved value stays and applies again once
        // unlocked. Whatever applies it decides the locked effect itself (value() still returns the saved value).
        std::function<bool()> locked;
        int lockedValue = 0;
        std::string lockedNote;

        // Key: 0 (unbound) is a valid value. A control binding; the menu clears one with a right click.
        bool optional = false;
        // Key: keys of one group are exclusive - binding a key another setting of the group has takes it from that
        // setting when that one is optional, and is refused otherwise (e.g. the menu key).
        std::string group;
        // Key: a second binding of the same action, drawn in this row beside the first (its own Spec, `hidden`).
        // Toggle: an Integer drawn as a slider beside the switch (a mod's priority), its own Spec, `hidden`.
        std::string partner;
        bool hidden = false; // no row of its own (a partner); still saved, reset with its page
        // Action: the button's text, and whether it asks for a second click first.
        std::string button;
        bool confirm = false;
    };

    void add(Spec spec);
    const std::vector<Spec> &all();
    std::vector<std::string> pages(); // in registration order

    int value(const std::string &key);         // the default for unknown keys / before load
    bool set(const std::string &key, int v);   // validates, saves vcs.ini, calls onChange (Live); false if refused
    bool overridden(const std::string &key);   // set by its environment variable at load (the menu shows it read-only)
    bool changedSinceStart(const std::string &key); // Restart settings: "applies after restart"

    void load(const std::filesystem::path &ini);
    void keepFile(); // after load: changes last for this run only, vcs.ini is never rewritten (hidden test runs)
    void applyAll();
    void refresh(); // takes the live values of settings with `read` (the menu calls it when it opens)

    // Saved state that is not a setting (no menu entry, no validation): text values in vcs.ini, e.g. the window
    // placement (window.*). Main thread, after load().
    std::string state(const std::string &key); // "" when absent
    void setState(const std::vector<std::pair<std::string, std::string>> &values); // one rewrite of vcs.ini
}
