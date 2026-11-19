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
    require(std::ranges::count_if(catalog.scenarios, [&](const auto& pair) { return pair.second.world == valley; }) == 3 &&
            catalog.scenarios.size() == 5 && catalog.issues.size() == 3,
            "Launcher failed to associate shared worlds, handle errors or deduplicate paths");
    const auto& field = catalog.scenarios.at(fs::canonical(bundled / "valley-demo.owscenario"));
    require(field.name == "Connecticut Valley - Field Deployment" &&
            std::ranges::any_of(field.details, [](const auto& row) { return row.find("field depots") != std::string::npos; }) &&
            field.commanders == std::set{FactionId::west, FactionId::east} &&
            std::ranges::find(field.details, "Time limit 24 h") != field.details.end() &&
            std::ranges::any_of(field.details, [](const auto& row) { return row.starts_with("WEST objective"); }),
            "Scenario summary omitted its authored factions, timing or objectives");
    const auto& junction = catalog.scenarios.at(fs::canonical(bundled / "valley-junction.owscenario"));
    require(junction.world == valley && junction.name == "Connecticut Valley - Valley Junction" &&
            junction.commanders == field.commanders, "Launcher omitted the advanced campaign or its roles");
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

void test_valley_junction(const fs::path& bundled)
{
    auto campaign = open_campaign_scenario(bundled / "valley-junction.owscenario", {}, 42);
    require(campaign.units().size() == 12 && campaign.carriers().size() == 4 &&
            campaign.shipments().size() == 8, "Valley Junction deployment did not load");
    campaign.advance_ticks(4);
    for (const auto side : {FactionId::west, FactionId::east}) {
        const auto view = campaign.faction_view(side, false);
        require(std::ranges::any_of(view.missions, [](const Mission& mission) {
                    return !mission.manual && mission.kind == MissionKind::attack &&
                        mission.target == SectorId{121} && mission.ground_journey && mission.launch_tick;
                }), "Both sides must launch autonomous geographic operations for the shared objective");
    }
}

void test_campaign_settings(const fs::path& root, const fs::path& bundled)
{
    const auto path = bundled / "valley-junction.owscenario";
    const auto world = bundled / "valley-world.owgeo";
    const auto scenario_before = bytes(path);
    const auto world_before = bytes(world);
    const auto base = read_geographic_scenario(path);
    const auto saves = root / "settings";
    {
        auto original = open_campaign_scenario(path);
        CampaignState defaults(customize_scenario(base, path, {}), 0);
        require(!defaults.setup() && bytes(save_new_checkpoint(defaults, saves)) ==
                bytes(save_new_checkpoint(original, saves)), "Default options changed the authored campaign");
    }

    CampaignOptions options;
    options.strength = {StartingPreset::reduced, StartingPreset::increased};
    options.stocks = {StartingPreset::reduced, StartingPreset::increased};
    options.seed = 42;
    options.duration_minutes = 120;
    options.autonomous_orders = 1;
    auto resolved = customize_scenario(base, path, options);

    const auto equipment_in = [](const ScenarioDefinition& scenario, UnitId unit, EquipmentTypeId type) {
        return std::ranges::count_if(scenario.equipment, [&](const auto& item) {
            return item.unit == unit && item.type == type;
        });
    };
    require(equipment_in(base, UnitId{2}, EquipmentTypeId{1}) == 3 &&
            equipment_in(resolved, UnitId{2}, EquipmentTypeId{1}) == 2 &&
            equipment_in(base, UnitId{102}, EquipmentTypeId{101}) == 2 &&
            equipment_in(resolved, UnitId{102}, EquipmentTypeId{101}) == 3 &&
            equipment_in(resolved, UnitId{6}, EquipmentTypeId{6}) == 2 &&
            resolved.equipment_types.size() == base.equipment_types.size(),
            "Strength presets failed to adjust repeated ready ground equipment independently");
    require(resolved.combat.time_limit_seconds == 7200 && resolved.operations.max_autonomous_missions == 1,
            "The time and autonomous-order settings were not passed to the initializer");

    CampaignOptions opposite;
    opposite.stocks = {StartingPreset::increased, StartingPreset::reduced};
    opposite.strength = {StartingPreset::increased, StartingPreset::reduced};
    CampaignState changed(std::move(resolved), options.seed);
    CampaignState reverse(customize_scenario(base, path, opposite), opposite.seed);
    constexpr auto fuel = static_cast<std::size_t>(Resource::fuel);
    require(changed.stockpiles().at(StockpileId{1}).quantity.at(fuel) == 225 &&
            changed.stockpiles().at(StockpileId{101}).quantity.at(fuel) == 340 &&
            changed.stockpiles().at(StockpileId{1}).reserved.at(fuel) == 0 &&
            reverse.stockpiles().at(StockpileId{1}).quantity.at(fuel) == 340 &&
            reverse.stockpiles().at(StockpileId{101}).quantity.at(fuel) == 225 &&
            changed.shipments().size() == base.initial_supply_requests.size() &&
            changed.carriers().at(CarrierId{1}).fuel == 35,
            "Stock presets changed reservations or carrier fuel, or two campaign setups shared state");

    const auto rejects = [&](const ScenarioDefinition& scenario, const fs::path& source, CampaignOptions invalid) {
        try {
            (void)customize_scenario(scenario, source, invalid);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    CampaignOptions invalid;
    invalid.duration_minutes = 60;
    require(rejects(base, path, invalid), "Time limit accepted an unreachable scenario victory duration");
    invalid = {};
    invalid.autonomous_orders = 1;
    auto without_planner = base;
    without_planner.operations.planner_interval_ticks = 0;
    require(rejects(without_planner, path, invalid), "Planner limit accepted a scenario without automatic planning");
    invalid = {};
    invalid.strength[0] = static_cast<StartingPreset>(9);
    require(rejects(base, path, invalid), "Invalid strength preset accepted");

    // Relative source references keep equivalent packages reproducible after relocation.
    const auto relocated = root / "relocated";
    fs::create_directory(relocated);
    const auto relocated_path = relocated / path.filename();
    fs::copy_file(path, relocated_path);
    fs::copy_file(world, relocated / world.filename());
    CampaignState duplicate(customize_scenario(read_geographic_scenario(relocated_path), relocated_path, options),
                            options.seed);
    changed.advance_ticks(4);
    duplicate.advance_ticks(4);
    for (const auto faction : {FactionId::west, FactionId::east}) {
        require(changed.faction_view(faction, false).missions.size() == 1,
                "Automatic order limit did not affect early planning");
    }
    const auto checkpoint = save_new_checkpoint(duplicate, saves);
    fs::remove_all(relocated);
    auto resumed = CampaignState::load(checkpoint);
    require(resumed.setup() == CampaignSetupRecord{"valley-junction.owscenario", "valley-world.owgeo", options} &&
            resumed.clock().tick() == 4 && bytes(checkpoint) == bytes(save_new_checkpoint(changed, saves)),
            "Customized setup was not preserved or was not deterministic");
    resumed.advance_ticks(3);
    changed.advance_ticks(3);
    require(bytes(save_new_checkpoint(resumed, saves)) == bytes(save_new_checkpoint(changed, saves)),
            "Customized campaign diverged after resumption");
    require(bytes(path) == scenario_before && bytes(world) == world_before, "Campaign settings edited a source file");

    CampaignOptions doctrines;
    doctrines.seed = 42;
    doctrines.doctrines = {StrategicDoctrine::offensive, StrategicDoctrine::defensive};
    auto doctrinal_setup = customize_scenario(base, path, doctrines);
    require(doctrinal_setup.operations.doctrines == doctrines.doctrines &&
            rejects(without_planner, path, doctrines),
            "Doctrine choices were not applied independently or accepted a disabled planner");
    CampaignState doctrinal(std::move(doctrinal_setup), doctrines.seed);
    const auto doctrine_checkpoint = save_new_checkpoint(doctrinal, saves);
    auto resumed_doctrine = CampaignState::load(doctrine_checkpoint);
    require(resumed_doctrine.setup() && resumed_doctrine.setup()->options == doctrines &&
            bytes(doctrine_checkpoint) == bytes(save_new_checkpoint(resumed_doctrine, saves)),
            "Checkpoint lost doctrine selections");

    CampaignOptions seed_only;
    seed_only.seed = 22;
    auto direct = open_campaign_scenario(path, {}, seed_only.seed);
    CampaignState seed_from_launcher(customize_scenario(base, path, seed_only), seed_only.seed);
    require(bytes(save_new_checkpoint(direct, saves)) == bytes(save_new_checkpoint(seed_from_launcher, saves)),
            "Direct authored launch differs from launcher with the same seed");

    const auto* legacy = std::getenv("LEGACY_SETUP_CHECKPOINT");
    require(legacy, "Missing schema 6 checkpoint fixture");
    auto old = CampaignState::load(legacy);
    CampaignState legacy_equivalent(combat_example_scenario(), 42);
    old.advance_ticks(8);
    legacy_equivalent.advance_ticks(8);
    require(!old.setup() && bytes(save_new_checkpoint(old, saves)) ==
            bytes(save_new_checkpoint(legacy_equivalent, saves)), "Schema 6 checkpoint resumes differently");
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
        test_valley_junction(fixtures);
        test_campaign_settings(files.directory, fixtures);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
