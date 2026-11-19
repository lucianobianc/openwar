#include "command.hpp"
#include "launcher_model.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {

namespace fs = std::filesystem;
using namespace openwar;

void require(bool ok, std::string_view message)
{
    if (!ok) {
        throw std::runtime_error(std::string(message));
    }
}

class TestFiles {
public:
    TestFiles() : directory(fs::temp_directory_path() /
        ("openwar-launcher-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        if (!fs::create_directory(directory))
            throw std::runtime_error("Cannot create launcher test directory");
    }

    ~TestFiles()
    {
        std::error_code error;
        fs::remove_all(directory, error);
    }

    const fs::path directory;
};

std::string bytes(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Cannot read launcher test checkpoint");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void test_discovery(const fs::path& root, const fs::path& bundled)
{
    const auto worlds = root / "Third party";
    fs::create_directory(worlds);
    const auto custom = worlds / "other-world.owgeo";
    {
        std::ofstream output(custom);
        output << "OWGEO 2\ngrid 4 3 2000000 200000\nprovenance "
               << std::quoted(R"({"name":"S\u00e3o Valley","epsg":32618})") << "\ntiles\n";
        // The header is valid even though the intentionally missing raster prevents starting it.
    }
    std::ofstream(worlds / "invalid.owscenario") << "OWSCENARIO 99\n";
    std::ofstream(worlds / "broken.owgeo") << "OWGEO 8\n";
    std::ofstream(worlds / "missing-world.owscenario") << "OWSCENARIO 1\nworld \"missing.owgeo\"\n";

    CampaignCatalog catalog;
    const std::array roots{worlds, bundled.parent_path(), worlds / ".", root / "absent"};
    catalog.discover(roots);
    const auto valley = fs::canonical(bundled / "valley-world.owgeo");
    const auto other = fs::canonical(custom);
    require(catalog.worlds.size() == 2 && catalog.worlds.at(valley).name == "Connecticut Valley" &&
            catalog.worlds.at(other).name == "São Valley", "Launcher did not discover multiple named geographic worlds");
    require(std::ranges::find(catalog.worlds.at(other).metadata, "8 x 6 km") != catalog.worlds.at(other).metadata.end() &&
            std::ranges::find(catalog.worlds.at(other).metadata, "EPSG 32618") != catalog.worlds.at(other).metadata.end(),
            "Header inspection did not retain extent and geographic reference");
    require(std::ranges::count_if(catalog.scenarios, [&](const auto& pair) { return pair.second.world == valley; }) == 2 &&
            catalog.scenarios.size() == 4 && catalog.issues.size() == 3,
            "Launcher failed to associate shared worlds, handle errors or deduplicate paths");
    const auto& field = catalog.scenarios.at(fs::canonical(bundled / "valley-demo.owscenario"));
    require(field.name == "Connecticut Valley - Field Deployment" &&
            std::ranges::any_of(field.details, [](const auto& row) { return row.find("field depots") != std::string::npos; }) &&
            field.commanders == std::set{FactionId::west, FactionId::east} &&
            std::ranges::find(field.details, "Time limit 24 h") != field.details.end() &&
            std::ranges::any_of(field.details, [](const auto& row) { return row.starts_with("WEST objective"); }),
            "Scenario summary omitted its authored factions, timing or objectives");
    require(catalog.add(custom) == other, "Selecting the same world again duplicated its catalog entry");
    try {
        (void)read_geographic_world(other);
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("Full loading accepted a world with a missing raster");
}

void test_start_and_resume(const fs::path& root, const fs::path& bundled)
{
    constexpr auto west = FactionId::west;
    constexpr auto east = FactionId::east;
    const auto copied = root / "copied";
    fs::create_directory(copied);
    fs::copy_file(bundled / "valley-world.owgeo", copied / "valley-world.owgeo");
    fs::copy_file(bundled / "valley-demo.owscenario", copied / "valley-demo.owscenario");
    const auto path = copied / "valley-demo.owscenario";
    auto campaign = open_campaign_scenario(path, west);
    CampaignState direct(read_geographic_scenario(path, west), 0);
    CommandSession observer(campaign), commander(campaign, west), opponent(campaign, east);
    require(observer.allows_perspective() && !observer.permissions().may_issue_orders &&
            commander.allows_perspective() && commander.permissions().may_issue_orders &&
            !opponent.allows_perspective() && !opponent.permissions().may_issue_orders,
            "Startup roles bypassed command authorization");

    campaign.advance_ticks(7);
    direct.advance_ticks(7);
    const auto saves = root / "saves";
    const auto first = save_new_checkpoint(campaign, saves);
    const auto second = save_new_checkpoint(direct, saves);
    require(first != second && bytes(first) == bytes(second), "GUI setup diverged from direct loading or replaced an existing save");
    fs::remove_all(copied);
    auto resumed = CampaignState::load(first);
    require(resumed.clock().tick() == 7 && CommandSession(resumed, west).allows_perspective() &&
            !CommandSession(resumed, east).allows_perspective(),
            "Checkpoint needed its source files or changed command faction");
    resumed.advance_ticks(20);
    direct.advance_ticks(20);
    require(bytes(save_new_checkpoint(resumed, saves)) == bytes(save_new_checkpoint(direct, saves)),
            "Checkpoint resumption diverged from uninterrupted execution");
    auto procedural = open_campaign_scenario(bundled.parent_path() / "procedural.toml");
    auto combined = open_campaign_scenario(bundled / "connecticut-valley.owgeo", east);
    require(procedural.generator_version() != 0 && !CommandSession(procedural, east).allows_perspective() &&
            combined.geographic_world() && CommandSession(combined, east).permissions().may_issue_orders,
            "Launcher changed procedural or combined-demo compatibility");
}

}  // namespace

int main()
{
    try {
        const auto* fixtures = std::getenv("VALLEY_FIXTURES");
        require(fixtures, "Missing launcher fixture directory");
        const TestFiles files;
        test_discovery(files.directory, fixtures);
        test_start_and_resume(files.directory, fixtures);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
