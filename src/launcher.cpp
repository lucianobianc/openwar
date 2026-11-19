#include "launcher.hpp"

#include "command.hpp"
#include "launcher_model.hpp"

#include <raylib.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace openwar {
namespace fs = std::filesystem;
namespace {

constexpr Color background{15, 22, 33, 255};
constexpr Color panel{24, 33, 47, 255};
constexpr Color border{47, 62, 83, 255};
constexpr Color ink{231, 240, 250, 255};
constexpr Color muted{161, 177, 196, 255};
constexpr Color accent{86, 211, 184, 255};
constexpr Color warning{255, 198, 139, 255};
constexpr int row_height = 50;

struct Entry {
    std::string title;
    std::vector<std::string> details{};
    fs::path path{};
};

struct Choice {
    std::optional<std::size_t> row{};
    fs::path dropped{};
};

void text(const std::string& value, int x, int y, int size, Color color, int width)
{
    if (MeasureText(value.c_str(), size) <= width) {
        DrawText(value.c_str(), x, y, size, color);
        return;
    }
    std::string clipped = value;
    while (!clipped.empty() && MeasureText((clipped + "...").c_str(), size) > width) {
        do {
            const auto byte = static_cast<unsigned char>(clipped.back());
            clipped.pop_back();
            if ((byte & 0xc0) != 0x80)
                break;
        } while (!clipped.empty());
    }
    DrawText((clipped + "...").c_str(), x, y, size, color);
}

void detail_text(std::string_view value, int x, int& y, int width, int bottom, int size, Color color)
{
    std::string line;
    while (!value.empty() && y + size < bottom) {
        const auto word = value.substr(0, value.find(' '));
        const auto next = line.empty() ? std::string(word) : line + " " + std::string(word);
        if (!line.empty() && MeasureText(next.c_str(), size) > width) {
            text(line, x, y, size, color, width);
            y += size + 7;
            line = word;
        } else {
            line = next;
        }
        value.remove_prefix(std::min(value.size(), word.size() + 1));
    }
    if (!line.empty() && y + size < bottom) {
        text(line, x, y, size, color, width);
        y += size + 7;
    }
}

void frame(std::string_view title, std::string_view hint)
{
    ClearBackground(background);
    const int width = GetScreenWidth() - 72;
    text("OpenWar", 36, 30, 30, ink, width);
    text(std::string(title), 36, 80, 24, accent, width);
    text(std::string(hint), 36, 112, 15, muted, width);
}

Choice menu(std::string_view title, std::string_view hint, std::span<const Entry> entries,
            std::string_view message = {}, bool back = true, std::size_t focus = 0)
{
    std::size_t first = 0;
    while (!WindowShouldClose()) {
        const int width = GetScreenWidth();
        const int left_width = width * 45 / 100;
        const int height = GetScreenHeight();
        const int top = 151;
        const auto visible = static_cast<std::size_t>(std::max(1, (height - top - 109) / row_height));
        const auto count = entries.size();
        const auto mouse = GetMousePosition();
        const auto movement = GetMouseDelta();
        const auto wheel = GetMouseWheelMove();
        const auto pressed = [&](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
        Choice result;
        bool done = false;
        const Rectangle back_button{36, static_cast<float>(height - 59), 100, 30};
        const bool back_hover = CheckCollisionPointRec(mouse, back_button);
        if (IsKeyPressed(KEY_ESCAPE) || (back && back_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))) {
            done = true;
        } else if (IsFileDropped()) {
            const auto files = LoadDroppedFiles();
            if (files.count) {
                result.dropped = fs::u8path(files.paths[0]);
                done = true;
            }
            UnloadDroppedFiles(files);
        }
        if (count) {
            if ((pressed(KEY_UP) || wheel > 0) && focus > 0)
                --focus;
            if ((pressed(KEY_DOWN) || wheel < 0) && focus + 1 < count)
                ++focus;
            if (pressed(KEY_HOME))
                focus = 0;
            if (pressed(KEY_END))
                focus = count - 1;
            first = std::min(first, focus);
            if (focus >= first + visible)
                first = focus - visible + 1;
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
                result.row = focus;
                done = true;
            }
        }

        BeginDrawing();
        frame(title, hint);
        const int side = left_width + 56;
        const int side_width = width - side - 36;
        const int stop = height - 97;
        DrawRectangle(side, top, side_width, stop - top, panel);
        for (std::size_t row = 0; row < visible && first + row < count; ++row) {
            const auto index = first + row;
            const int y = top + static_cast<int>(row) * row_height;
            const Rectangle bounds{36, static_cast<float>(y), static_cast<float>(left_width), row_height - 6};
            const bool hover = CheckCollisionPointRec(mouse, bounds);
            if (hover && (movement.x != 0 || movement.y != 0))
                focus = index;
            if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                result.row = index;
                done = true;
            }
            DrawRectangleRec(bounds, index == focus ? Color{34, 74, 73, 255} : hover ? border : panel);
            DrawRectangleLinesEx(bounds, 1, index == focus ? accent : border);
            text(entries[index].title, 50, y + 13, 17, index == focus ? accent : ink, left_width - 26);
        }
        if (count) {
            auto y = top + 18;
            detail_text(entries[focus].title, side + 16, y, side_width - 32, stop, 20, ink);
            y += 5;
            for (const auto& detail : entries[focus].details) {
                detail_text(detail, side + 16, y, side_width - 32, stop - 5, 15, muted);
            }
            if (entries[focus].details.empty() && !entries[focus].path.empty()) {
                detail_text(entries[focus].path.string(), side + 16, y, side_width - 32, stop - 5, 15, muted);
            }
        }
        if (back) {
            DrawRectangleRec(back_button, back_hover ? border : panel);
            DrawRectangleLinesEx(back_button, 1, border);
            text("Back (Esc)", 46, height - 50, 15, ink, 83);
        }
        if (!message.empty()) {
            auto y = height - 85;
            detail_text(message, back ? 154 : 36, y, width - (back ? 190 : 72), height - 13, 15, warning);
        } else {
            const auto keys = count > visible ? "Up / Down or scroll to browse, Enter to select" :
                "Up / Down to focus, Enter to select";
            text(keys, back ? 154 : 36, height - 48, 14, muted, width - 190);
        }
        EndDrawing();
        if (done)
            return result;
    }
    return {};
}

fs::path environment_path(const char* name)
{
    const auto* value = std::getenv(name);
    return value && *value ? fs::u8path(value) : fs::path{};
}

bool path_is_directory(const fs::path& path)
{
    std::error_code error;
    return fs::is_directory(path, error);
}

bool recognized(const fs::path& path, bool checkpoint)
{
    const auto extension = path.extension();
    return checkpoint ? extension == ".ows" : extension == ".owgeo" || extension == ".owscenario" || extension == ".toml";
}

std::optional<std::string> enter_text(std::string_view title, std::string_view hint, std::string input = {})
{
    while (!WindowShouldClose()) {
        const bool submit = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);
        const bool back = IsKeyPressed(KEY_ESCAPE);
        if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) && IsKeyPressed(KEY_V)) {
            if (const auto* paste = GetClipboardText())
                input = paste;
        } else {
            for (auto codepoint = GetCharPressed(); codepoint && input.size() < 4096; codepoint = GetCharPressed()) {
                int size = 0;
                const auto* bytes = CodepointToUTF8(codepoint, &size);
                input.append(bytes, static_cast<std::size_t>(size));
            }
        }
        if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && !input.empty()) {
            auto last = input.size() - 1;
            while (last > 0 && (static_cast<unsigned char>(input[last]) & 0xc0) == 0x80)
                --last;
            input.resize(last);
        }
        BeginDrawing();
        frame(title, hint);
        const auto width = GetScreenWidth() - 72;
        DrawRectangle(36, 160, width, 45, panel);
        DrawRectangleLines(36, 160, width, 45, accent);
        auto tail = input;
        while (!tail.empty() && MeasureText(tail.c_str(), 18) > width - 24)
            tail.erase(0, 1);
        text(tail, 48, 172, 18, ink, width - 24);
        text("Enter to confirm / Esc to go back", 36, 227, 15, muted, width);
        EndDrawing();
        if (back)
            return {};
        if (submit && !input.empty()) {
            while (!input.empty() && (input.back() == '\r' || input.back() == '\n'))
                input.pop_back();
            if (!input.empty())
                return input;
        }
    }
    return {};
}

#ifndef _WIN32
bool native_picker_available()
{
    return std::system("command -v zenity >/dev/null 2>&1") == 0;
}

fs::path native_picker()
{
    // No user input is interpolated into the shell command.
    auto* picker = popen("zenity --file-selection --title='OpenWar - Open file' 2>/dev/null", "r");
    if (!picker)
        return {};
    char buffer[4096];
    std::string value;
    while (std::fgets(buffer, sizeof(buffer), picker))
        value += buffer;
    const auto status = pclose(picker);
    if (status != 0)
        return {};
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
        value.pop_back();
    return fs::u8path(value);
}
#endif

fs::path browse(fs::path directory, bool checkpoint)
{
    if (!path_is_directory(directory)) {
#ifdef _WIN32
        directory = environment_path("USERPROFILE");
#else
        directory = environment_path("HOME");
#endif
    }
    if (!path_is_directory(directory))
        directory = fs::path(GetApplicationDirectory());
    directory = fs::absolute(directory).lexically_normal();
    std::string error;
#ifndef _WIN32
    const bool picker = native_picker_available();
#else
    const bool picker = false;
#endif
    while (!WindowShouldClose()) {
        std::vector<Entry> entries{{"Enter or paste a path"}};
        if (picker)
            entries.push_back({"Open system file picker"});
        if (directory.has_parent_path() && directory.parent_path() != directory)
            entries.push_back({"Up one folder", {}, directory.parent_path()});
        std::error_code ec;
        std::set<fs::path> folders, files;
        for (fs::directory_iterator it(directory, ec); !ec && it != fs::directory_iterator{}; it.increment(ec)) {
            std::error_code status;
            if (it->is_directory(status))
                folders.insert(it->path());
            else if (it->is_regular_file(status) && recognized(it->path(), checkpoint))
                files.insert(it->path());
        }
        for (const auto& folder : folders)
            entries.push_back({folder.filename().string() + "/", {}, folder});
        for (const auto& file : files)
            entries.push_back({file.filename().string(), {}, file});
        auto choice = menu(checkpoint ? "Continue saved campaign" : "Find campaign content", directory.string(),
                           entries, error.empty() && ec ? ec.message() : error);
        if (!choice.row && choice.dropped.empty())
            return {};
        auto path = choice.row ? entries[*choice.row].path : std::move(choice.dropped);
        if (choice.row == 0) {
            if (const auto entered = enter_text("Open a path", "Type a file or folder path, or paste it with Ctrl+V."))
                path = fs::u8path(*entered);
#ifndef _WIN32
        } else if (picker && choice.row == 1) {
            path = native_picker();
#endif
        }
        if (path.empty())
            continue;
        if (path.is_relative())
            path = directory / path;
        if (path_is_directory(path)) {
            directory = path.lexically_normal();
            error.clear();
        } else if (!checkpoint && !recognized(path, false)) {
            error = "Choose a world or scenario file.";
        } else {
            return path;
        }
    }
    return {};
}

std::string_view strength_label(StartingPreset preset)
{
    constexpr std::array names{"Scenario original", "Reduced", "Reinforced"};
    return names[static_cast<std::size_t>(preset)];
}

std::string_view stock_label(StartingPreset preset)
{
    constexpr std::array names{"Scenario original", "Lower (75%)", "Higher (125% within capacity)"};
    return names[static_cast<std::size_t>(preset)];
}

struct OperatorRole {
    std::optional<FactionId> faction;
};

std::optional<OperatorRole> select_role(std::string_view name, const std::set<FactionId>& commanders, bool resume,
                                       std::span<const std::string> setup = {})
{
    std::vector<OperatorRole> roles{{{}}};
    std::vector<Entry> entries{{"Observer", {"Read-only campaign view."}}};
    for (const auto faction : commanders) {
        roles.push_back({faction});
        entries.push_back({std::string(faction_name(faction)) + " commander", {
            "Issue orders and view intelligence for " + std::string(faction_name(faction)) + "."
        }});
    }
    for (auto& entry : entries)
        entry.details.insert(entry.details.end(), setup.begin(), setup.end());
    const auto choice = menu(resume ? "Resume as" : "Start as", name, entries);
    return choice.row ? std::optional{roles[*choice.row]} : std::nullopt;
}

std::optional<LaunchSelection> continue_campaign(const fs::path& saves, fs::path file, std::string& error)
{
    if (file.empty())
        file = browse(saves, true);
    if (file.empty())
        return {};
    try {
        if (!fs::is_regular_file(file))
            throw std::runtime_error("Checkpoint file not found: " + file.string());
        auto campaign = CampaignState::load(file);
        std::set<FactionId> commanders;
        for (const auto faction : {FactionId::west, FactionId::east}) {
            if (CommandSession(campaign, faction).allows_perspective())
                commanders.insert(faction);
        }
        const auto name = file.filename().string() + " / tick " + std::to_string(campaign.clock().tick());
        std::vector<std::string> settings;
        if (campaign.setup()) {
            const auto& record = *campaign.setup();
            const auto& options = record.options;
            settings = {"Scenario " + record.scenario, "World " + record.world,
                "Seed " + std::to_string(options.seed),
                "WEST strength / " + std::string(strength_label(options.strength[0])),
                "EAST strength / " + std::string(strength_label(options.strength[1])),
                "WEST stocks / " + std::string(stock_label(options.stocks[0])),
                "EAST stocks / " + std::string(stock_label(options.stocks[1]))};
            if (options.duration_minutes)
                settings.push_back("Time limit " + std::to_string(*options.duration_minutes) + " min");
            if (options.autonomous_orders)
                settings.push_back("Autonomous orders " + std::to_string(*options.autonomous_orders) + " per faction");
        }
        if (const auto role = select_role(name, commanders, true, settings))
            return LaunchSelection{std::move(campaign), role->faction};
    } catch (const std::exception& failure) {
        error = failure.what();
    }
    return {};
}

void choose_preset(std::string_view title, StartingPreset& value, bool strength)
{
    const std::array choices{
        Entry{"Scenario original", {"Use the authored starting values."}},
        strength ? Entry{"Reduced", {"Remove one ready ground item from each repeated occupying type per formation."}} :
            Entry{"Lower (75%)", {"Reduce initial unreserved stock quantities by 25%, rounding down."}},
        strength ? Entry{"Reinforced", {"Add one ready ground item to each repeated occupying type per formation."}} :
            Entry{"Higher (125%)", {"Increase initial unreserved stock quantities by up to 25% within capacity."}}
    };
    const auto choice = menu(title, "Select a starting preset.", choices, {}, true, static_cast<std::size_t>(value));
    if (choice.row)
        value = static_cast<StartingPreset>(*choice.row);
}

std::optional<LaunchSelection> start_with_settings(const Entry& entry, const ScenarioListing& listing)
{
    const auto base = read_geographic_scenario(entry.path);
    const auto planner = base.operations.enabled && base.operations.planner_interval_ticks;
    CampaignOptions options;
    std::string error;
    std::size_t focus = 0;
    while (!WindowShouldClose()) {
        const auto original_duration = base.combat.time_limit_seconds ?
            std::to_string(*base.combat.time_limit_seconds / 60) + " min" : "No time limit";
        const auto duration = options.duration_minutes ?
            std::to_string(*options.duration_minutes) + " min" : original_duration + " (scenario)";
        const auto orders = !planner ? std::string("Disabled by scenario") : options.autonomous_orders ?
            std::to_string(*options.autonomous_orders) + " per faction" :
            std::to_string(base.operations.max_autonomous_missions) + " per faction (scenario)";
        const std::array entries{
            Entry{"Continue to operator role", {"Validate these settings, then choose Observer or a commander.",
                "Restore scenario defaults to start with the original forces, stocks, schedule and seed."}},
            Entry{"WEST strength / " + std::string(strength_label(options.strength[0])),
                {"Changes ready ground equipment that can occupy sectors and occurs at least twice in a formation."}},
            Entry{"EAST strength / " + std::string(strength_label(options.strength[1])),
                {"Changes ready ground equipment that can occupy sectors and occurs at least twice in a formation."}},
            Entry{"WEST stocks / " + std::string(stock_label(options.stocks[0])),
                {"Adjusts fixed and mobile WEST stock quantities within their original capacity."}},
            Entry{"EAST stocks / " + std::string(stock_label(options.stocks[1])),
                {"Adjusts fixed and mobile EAST stock quantities within their original capacity."}},
            Entry{"Random seed / " + std::to_string(options.seed),
                {"Repeat the same seed, settings and commands to reproduce a campaign. The launcher default is 0."}},
            Entry{"Time limit / " + duration, {"Sets the maximum simulated duration. Victory can occur earlier."}},
            Entry{"Autonomous orders / " + orders, {planner ? "Sets the maximum active automatic orders per faction." :
                "Automatic planning is disabled for this scenario."}},
            Entry{"Restore scenario defaults", {"Reset every setting on this screen."}}
        };
        const auto choice = menu("Campaign settings", entry.title, entries, error, true, focus);
        if (!choice.row)
            return {};
        focus = *choice.row;
        error.clear();
        try {
            switch (focus) {
            case 0: {
                auto resolved = customize_scenario(base, entry.path, options);
                if (const auto role = select_role(entry.title, listing.commanders, false)) {
                    resolved.operations.player = role->faction;
                    return LaunchSelection{CampaignState(std::move(resolved), options.seed), role->faction};
                }
                break;
            }
            case 1:
            case 2:
                choose_preset(focus == 1 ? "WEST starting strength" : "EAST starting strength",
                    options.strength[focus - 1], true);
                break;
            case 3:
            case 4:
                choose_preset(focus == 3 ? "WEST starting stocks" : "EAST starting stocks",
                    options.stocks[focus - 3], false);
                break;
            case 5: {
                const auto input = enter_text("Random seed", "Enter a whole number from 0 to 4294967295.",
                                              std::to_string(options.seed));
                if (input) {
                    std::uint32_t seed = 0;
                    const auto end = input->data() + input->size();
                    const auto parsed = std::from_chars(input->data(), end, seed);
                    if (parsed.ec != std::errc{} || parsed.ptr != end)
                        throw std::invalid_argument("Enter a whole number from 0 to 4294967295");
                    options.seed = seed;
                }
                break;
            }
            case 6: {
                const std::array choices{Entry{"Scenario original / " + original_duration}, Entry{"1 hour"},
                    Entry{"2 hours"}, Entry{"4 hours"}, Entry{"8 hours"}};
                constexpr std::array<std::optional<std::uint32_t>, 5> minutes{{{}, 60, 120, 240, 480}};
                if (const auto selected = menu("Time limit", "Choose the maximum simulated duration.", choices).row)
                    options.duration_minutes = minutes[*selected];
                break;
            }
            case 7: {
                if (!planner)
                    break;
                const std::array choices{Entry{"Scenario original"}, Entry{"One per faction"},
                    Entry{"Two per faction"}, Entry{"Three per faction"}};
                if (const auto selected = menu("Autonomous orders", "Choose the maximum active automatic orders.", choices).row)
                    options.autonomous_orders = *selected == 0 ? std::nullopt :
                        std::optional{static_cast<std::uint32_t>(*selected)};
                break;
            }
            case 8:
                options = {};
                focus = 0;
                break;
            }
        } catch (const std::exception& failure) {
            error = failure.what();
        }
    }
    return {};
}

std::optional<LaunchSelection> start_scenario(const Entry& entry, const ScenarioListing& scenario)
{
    if (entry.path.extension() == ".owscenario")
        return start_with_settings(entry, scenario);
    const auto role = select_role(entry.title, scenario.commanders, false);
    if (!role)
        return {};
    auto campaign = open_campaign_scenario(entry.path, role->faction);
    if (!CommandSession(campaign, role->faction).allows_perspective())
        throw std::runtime_error("Campaign does not grant command authority to that faction");
    return LaunchSelection{std::move(campaign), role->faction};
}

std::optional<LaunchSelection> select_scenario(const CampaignCatalog& catalog, const fs::path& world)
{
    std::string error;
    const auto title = world.empty() ? "Combined and procedural campaigns" : catalog.worlds.at(world).name;
    std::vector<Entry> entries;
    for (const auto& [file, listing] : catalog.scenarios) {
        if (listing.world == world)
            entries.push_back({listing.name, listing.details, file});
    }
    if (entries.empty()) {
        const std::array empty{Entry{"No associated military scenarios", {
            "Place an OWSCENARIO 1 file that references this world nearby, or return and locate an existing scenario."
        }}};
        menu(title, "Military scenarios", empty);
        return {};
    }
    while (!WindowShouldClose()) {
        const auto choice = menu(title, "Select a military scenario", entries, error);
        if (!choice.row)
            return {};
        const auto& entry = entries[*choice.row];
        try {
            if (auto selected = start_scenario(entry, catalog.scenarios.at(entry.path)))
                return selected;
        } catch (const std::exception& failure) {
            error = failure.what();
        }
    }
    return {};
}

std::optional<LaunchSelection> new_campaign(CampaignCatalog& catalog, const fs::path& data_directory, fs::path file)
{
    std::string error;
    while (!WindowShouldClose()) {
        try {
            if (!file.empty()) {
                const auto path = catalog.add(file);
                file.clear();
                error.clear();
                if (catalog.worlds.contains(path)) {
                    const std::array nearby{path.parent_path()};
                    catalog.discover(nearby, false);
                    if (auto selection = select_scenario(catalog, path))
                        return selection;
                } else {
                    const auto& listing = catalog.scenarios.at(path);
                    if (auto selection = start_scenario({listing.name, listing.details, path}, listing))
                        return selection;
                }
            }
        } catch (const std::exception& failure) {
            error = failure.what();
            file.clear();
        }
        std::vector<Entry> entries;
        for (const auto& [path, listing] : catalog.worlds) {
            auto details = listing.metadata;
            const auto count = std::ranges::count_if(catalog.scenarios, [&](const auto& scenario) {
                return scenario.second.world == path;
            });
            details.push_back(std::to_string(count) + (count == 1 ? " military scenario" : " military scenarios"));
            details.push_back(path.string());
            entries.push_back({listing.name, std::move(details), path});
        }
        const auto browse_index = entries.size();
        entries.push_back({"Browse for content...", {"Open a world or military scenario from disk. You can also drop a file onto this window."}});
        const auto legacy_index = entries.size();
        if (std::ranges::any_of(catalog.scenarios, [](const auto& scenario) { return scenario.second.world.empty(); }))
            entries.push_back({"Combined and procedural campaigns", {"Open an existing OWGEO 1 demo or a procedural configuration."}});
        const auto issues_index = entries.size();
        if (!catalog.issues.empty())
            entries.push_back({"Content diagnostics", catalog.issues});
        const auto choice = menu("Geographic worlds", "Choose a world to see its military scenarios.", entries, error);
        if (!choice.dropped.empty()) {
            file = choice.dropped;
        } else if (!choice.row) {
            return {};
        } else if (*choice.row < browse_index) {
            if (auto selection = select_scenario(catalog, entries[*choice.row].path))
                return selection;
        } else if (*choice.row == browse_index) {
            file = browse(data_directory / "scenarios", false);
        } else if (*choice.row < issues_index) {
            if (auto selection = select_scenario(catalog, {}))
                return selection;
        } else {
            std::vector<Entry> issues;
            for (std::size_t i = 0; i < catalog.issues.size(); ++i)
                issues.push_back({"Content issue " + std::to_string(i + 1), {catalog.issues[i]}});
            menu("Content diagnostics", "Select or focus an entry to inspect the error.", issues);
        }
    }
    return {};
}

}  // namespace

fs::path launcher_data_directory()
{
#ifdef _WIN32
    if (const auto local = environment_path("LOCALAPPDATA"); !local.empty())
        return local / "OpenWar";
    if (const auto home = environment_path("USERPROFILE"); !home.empty())
        return home / "OpenWar";
#else
    if (const auto xdg = environment_path("XDG_DATA_HOME"); xdg.is_absolute())
        return xdg / "openwar";
    if (const auto home = environment_path("HOME"); !home.empty())
        return home / ".local" / "share" / "openwar";
#endif
    return fs::path(GetApplicationDirectory()) / "data";
}

std::optional<LaunchSelection> show_launcher(const fs::path& data_directory, std::string error)
{
    CampaignCatalog catalog;
    bool catalog_loaded = false;
    while (!WindowShouldClose()) {
        const std::array entries{
            Entry{"New campaign", {"Choose a geographic world, its military scenario, and your role."}},
            Entry{"Continue saved campaign", {"Resume a checkpoint at its saved tick.", (data_directory / "saves").string()}},
            Entry{"Exit"}
        };
        const auto choice = menu("Campaign launcher", "Start or resume an OpenWar campaign.", entries, error, false);
        error.clear();
        if ((!choice.row && choice.dropped.empty()) || choice.row == 2)
            return {};
        if (choice.row == 1 || choice.dropped.extension() == ".ows") {
            if (auto selection = continue_campaign(data_directory / "saves", choice.dropped, error))
                return selection;
            continue;
        }
        if (!catalog_loaded) {
            const fs::path executable(GetApplicationDirectory());
            std::vector roots{data_directory / "scenarios", executable / "data"};
            const auto adjacent = executable / ".." / "share" / "openwar";
            const fs::path configured(OPENWAR_DATA_DIR);
            if (path_is_directory(adjacent)) {
                roots.push_back(adjacent);
            } else if (path_is_directory(configured)) {
                roots.push_back(configured);
            } else {
                roots.emplace_back(OPENWAR_SOURCE_DATA_DIR);
            }
            if (const auto extra = environment_path("OPENWAR_DATA_DIR"); !extra.empty())
                roots.push_back(extra);
            catalog.discover(roots);
            catalog_loaded = true;
        }
        if (auto selection = new_campaign(catalog, data_directory, choice.dropped))
            return selection;
    }
    return {};
}

}  // namespace openwar
