#include "scenario.hpp"
#include "command.hpp"
#include "realtime.hpp"

#include <charconv>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view usage = R"(Usage: openwar [--demo NAME | --scenario FILE | --load FILE] [--speed N] [--ticks N] [--events] [--seed N] [--save FILE]

Without a tick limit, the dashboard runs in real time until Ctrl+C.

Options
  --demo NAME  Select economy (default), logistics, maintenance, operations, combat, campaign, or procedural.
  --scenario FILE  Load an .owscenario, an OWGEO 1 demo or a procedural configuration.
  --inspect-world FILE  Inspect an OWGEO 2 world without starting a campaign.
  --load FILE      Resume a saved campaign.
  --save FILE      Write a checkpoint at the end of the run.
  --speed N    Pace the run at N simulated seconds per real second (default 60).
  --ticks N    Advance up to N additional ticks. Runs in batch mode unless speed is specified.
  --events     Show timestamped campaign events.
  --seed N     Override the seed (32-bit for fixed demos, 64-bit for procedural).
  --faction WEST|EAST  Use one faction's view in an operational checkpoint or demo.
  --order TYPE:SECTOR  Queue a recon, patrol, defend, reposition, or attack order.
  --ground-order TYPE:EAST,SOUTH,UP  Submit a ground order at a point in millimetres.
  --cancel ID         Cancel an eligible order before advancing time.
  --help       Show this help.

Examples
  openwar
  openwar --speed 600
  openwar --demo logistics --ticks 100
  openwar --demo maintenance --speed 600 --ticks 12 --events
  openwar --demo operations --faction WEST --order recon:2 --ticks 12 --events
  openwar --demo combat --ticks 30 --events
  openwar --demo campaign --seed 42 --ticks 3000
  openwar --scenario examples/procedural.toml --ticks 0
  openwar --demo campaign --seed 42 --ticks 600 --save campaign.ows
  openwar --load campaign.ows --ticks 600 --save campaign.ows
)";

enum class Demo { economy, logistics, maintenance, operations, combat, campaign, procedural };

Demo parse_demo(std::string_view name)
{
    if (name == "economy") {
        return Demo::economy;
    }
    if (name == "logistics") {
        return Demo::logistics;
    }
    if (name == "maintenance") {
        return Demo::maintenance;
    }
    if (name == "operations") {
        return Demo::operations;
    }
    if (name == "combat") {
        return Demo::combat;
    }
    if (name == "campaign") {
        return Demo::campaign;
    }
    if (name == "procedural") {
        return Demo::procedural;
    }
    throw std::invalid_argument("Unknown demonstration: " + std::string(name));
}

openwar::CampaignState make_campaign(Demo demo, std::uint64_t requested_seed,
                                     std::optional<openwar::FactionId> faction,
                                     const openwar::ProceduralSettings& procedural)
{
    using namespace openwar;
    const auto seed = static_cast<std::uint32_t>(requested_seed);
    switch (demo) {
    case Demo::economy:
        return CampaignState(example_scenario(), seed);
    case Demo::maintenance:
        return maintenance_example_campaign(seed);
    case Demo::operations:
        return operations_example_campaign(seed, faction);
    case Demo::combat:
        return CampaignState(combat_example_scenario(faction), seed);
    case Demo::campaign:
        return long_example_campaign(seed);
    case Demo::procedural:
        return procedural_campaign(procedural);
    case Demo::logistics: {
        CampaignState campaign(logistics_example_scenario(), seed);
        campaign.request_supply({ShipmentId{1}, StockpileId{1}, StockpileId{3}, Resource::fuel, 80});
        campaign.request_supply({ShipmentId{2}, StockpileId{1}, StockpileId{3}, Resource::fuel, 40});
        return campaign;
    }
    }
    throw std::logic_error("Unknown demonstration");
}

std::uint64_t parse_number(std::string_view text, std::string_view option)
{
    std::uint64_t value{};
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        throw std::invalid_argument("Invalid unsigned integer for " + std::string(option));
    }
    return value;
}

openwar::geography::Point parse_ground_target(std::string_view coordinates)
{
    openwar::geography::Point point;
    for (auto* axis : {&point.east, &point.south, &point.elevation}) {
        if (coordinates.empty()) {
            throw std::invalid_argument("Expected ground coordinates EAST,SOUTH,UP in millimetres");
        }
        const auto separator = coordinates.find(',');
        if ((axis == &point.elevation) != (separator == std::string_view::npos)) {
            throw std::invalid_argument("Expected ground coordinates EAST,SOUTH,UP in millimetres");
        }
        const auto component = coordinates.substr(0, separator);
        const auto end = component.data() + component.size();
        const auto result = std::from_chars(component.data(), end, *axis);
        if (result.ec != std::errc{} || result.ptr != end) {
            throw std::invalid_argument("Invalid ground coordinate in millimetres");
        }
        if (separator != std::string_view::npos) {
            coordinates.remove_prefix(separator + 1);
        }
    }
    return point;
}

void apply_order(openwar::CommandSession& session, std::string_view order, bool geographic)
{
    using namespace openwar;
    const auto separator = order.find(':');
    if (separator == std::string_view::npos) {
        throw std::invalid_argument(geographic ? "Expected ground order TYPE:EAST,SOUTH,UP" : "Expected order TYPE:SECTOR");
    }
    OrderTarget target;
    if (geographic) {
        target = parse_ground_target(order.substr(separator + 1));
    } else {
        const auto sector = parse_number(order.substr(separator + 1), "--order sector");
        if (sector > UINT32_MAX) {
            throw std::out_of_range("Order sector must fit in 32 bits");
        }
        target = SectorId{static_cast<std::uint32_t>(sector)};
    }
    for (const auto kind : {MissionKind::recon, MissionKind::patrol, MissionKind::defend,
                           MissionKind::reposition, MissionKind::attack}) {
        if (order.substr(0, separator) == mission_name(kind)) {
            const auto result = session.submit(kind, target);
            if (!result.mission) {
                throw std::invalid_argument(std::string(result.reason));
            }
            return;
        }
    }
    throw std::invalid_argument("Unknown order type");
}

void report(const openwar::CampaignState& campaign, std::uint64_t seed)
{
    const auto& world = campaign.world();
    std::cout << "Seed: " << seed << '\n'
              << "Tick: " << campaign.clock().tick() << '\n'
              << "Elapsed seconds: " << campaign.clock().elapsed_seconds() << '\n'
              << "World: " << world.width() << 'x' << world.height()
              << ", sector size " << world.sector_size_m() << " m\n";

    for (const auto& sector : world.sectors()) {
        const auto& state = campaign.sector_state(sector.id);
        std::cout << "Sector " << static_cast<std::uint32_t>(sector.id)
                  << " (" << sector.x << ',' << sector.y << ") "
                  << openwar::faction_name(state.controller)
                  << ' ' << openwar::terrain_name(sector.properties.terrain)
                  << ", traversable: " << (sector.properties.traversable ? "yes" : "no")
                  << ", objective points: " << state.objective_points
                  << ", neighbors:";
        for (const auto neighbor : sector.neighbors) {
            std::cout << ' ' << static_cast<std::uint32_t>(neighbor);
        }
        std::cout << '\n';
    }

    for (const auto& [id, stock] : campaign.stockpiles()) {
        std::cout << "Stockpile " << static_cast<std::uint32_t>(id)
                  << " in sector " << static_cast<std::uint32_t>(stock.sector)
                  << " (" << openwar::faction_name(stock.owner) << ")\n";
        for (std::size_t i = 0; i < stock.quantity.size(); ++i) {
            if (stock.capacity[i] != 0) {
                std::cout << "  " << openwar::resource_name(static_cast<openwar::Resource>(i))
                          << ": " << stock.quantity[i] << '/' << stock.capacity[i] << '\n';
            }
        }
    }
    for (const auto& state : campaign.installations()) {
        const auto& installation = state.installation;
        std::cout << "Installation " << static_cast<std::uint32_t>(installation.id)
                  << " (" << installation.name << "), stockpile "
                  << static_cast<std::uint32_t>(installation.stockpile)
                  << ", work: " << state.work << '/'
                  << std::uint64_t{installation.recipe.intervals_per_batch} * 100 << '\n';
    }
    std::cout << "Economy transactions: " << campaign.economy_ledger().size() << '\n';
    for (const auto& [id, shipment] : campaign.shipments()) {
        std::cout << "Shipment " << static_cast<std::uint32_t>(id) << ": "
                  << openwar::shipment_status_name(shipment.status)
                  << ", cargo on carrier: " << shipment.cargo << '\n';
    }
    openwar::cli::report_forces(std::cout, campaign);
}

void report_procedural_world(const openwar::CampaignState& campaign, bool preview)
{
    const auto& world = campaign.world();
    std::cout << "Procedural generator: " << campaign.generator_version() << '\n'
              << "World: " << world.width() << 'x' << world.height()
              << ", sector size " << world.sector_size_m() << " m\n";
    if (preview) {
        std::cout << "Terrain (. plains, F forest, ~ water)\n";
        for (const auto& sector : world.sectors()) {
            using openwar::Terrain;
            const auto terrain = sector.properties.terrain;
            std::cout << (terrain == Terrain::water ? '~' : terrain == Terrain::forest ? 'F' : '.');
            if (sector.x + 1 == world.width()) {
                std::cout << '\n';
            }
        }
    }
}

}  // namespace

int main(int argc, char* argv[])
{
    std::optional<std::uint64_t> seed_override;
    std::optional<std::filesystem::path> scenario_path;
    std::optional<std::filesystem::path> world_path;
    std::optional<std::filesystem::path> load_path;
    std::optional<std::filesystem::path> save_path;
    std::optional<std::uint64_t> ticks;
    std::optional<std::uint32_t> speed;
    std::optional<Demo> demo;
    std::optional<openwar::FactionId> faction;
    std::vector<std::pair<std::string_view, std::string_view>> commands;
    bool legacy_realtime = false;
    bool events = false;
    try {
        const auto select_demo = [&](Demo selection) {
            if (demo && demo != selection) {
                throw std::invalid_argument("Select one demonstration scenario");
            }
            demo = selection;
        };
        for (int i = 1; i < argc; ++i) {
            const std::string_view option = argv[i];
            if (option == "--help") {
                std::cout << usage;
                return 0;
            }
            if (option == "--logistics-demo") {
                select_demo(Demo::logistics);
                continue;
            }
            if (option == "--maintenance-demo") {
                select_demo(Demo::maintenance);
                continue;
            }
            if (option == "--realtime") {
                legacy_realtime = true;
                continue;
            }
            if (option == "--events") {
                events = true;
                continue;
            }
            if (option != "--demo" && option != "--seed" && option != "--ticks" && option != "--speed" &&
                option != "--faction" && option != "--order" && option != "--ground-order" && option != "--cancel" && option != "--scenario" &&
                option != "--load" && option != "--save" && option != "--inspect-world") {
                throw std::invalid_argument("Unknown option: " + std::string(option));
            }
            if (++i == argc) {
                throw std::invalid_argument("Missing value for " + std::string(option));
            }
            if (option == "--demo") {
                select_demo(parse_demo(argv[i]));
                continue;
            }
            if (option == "--scenario") {
                select_demo(Demo::procedural);
                if (scenario_path) {
                    throw std::invalid_argument("Select one scenario file");
                }
                scenario_path = argv[i];
                continue;
            }
            if (option == "--load" || option == "--save" || option == "--inspect-world") {
                auto& path = option == "--load" ? load_path : option == "--save" ? save_path : world_path;
                if (path) {
                    throw std::invalid_argument("Specify " + std::string(option) + " only once");
                }
                path = argv[i];
                continue;
            }
            if (option == "--faction") {
                const std::string_view name = argv[i];
                if (name == "WEST" || name == "west") {
                    faction = openwar::FactionId::west;
                } else if (name == "EAST" || name == "east") {
                    faction = openwar::FactionId::east;
                } else {
                    throw std::invalid_argument("Faction must be WEST or EAST");
                }
                continue;
            }
            if (option == "--order" || option == "--ground-order" || option == "--cancel") {
                commands.emplace_back(option, argv[i]);
                continue;
            }
            const auto value = parse_number(argv[i], option);
            if (option == "--seed") {
                seed_override = value;
            } else if (option == "--ticks") {
                ticks = value;
            } else {
                if (value == 0 || value > UINT32_MAX) {
                    throw std::out_of_range("Speed must be a positive 32-bit integer");
                }
                speed = static_cast<std::uint32_t>(value);
            }
        }
        if (world_path) {
            if (argc != 3)
                throw std::invalid_argument("--inspect-world takes one world and no campaign options");
            const auto world = openwar::read_geographic_world(*world_path);
            std::cout << "Geographic world: " << world.grid.width() << 'x' << world.grid.height()
                      << " sectors, " << world.map.roads.size() << " road segments, "
                      << world.map.waterways.size() << " waterways, " << world.map.site_features.size()
                      << " catalogued features\n";
            return 0;
        }
        if (load_path && (demo || seed_override)) {
            throw std::invalid_argument("--load cannot be combined with --demo, --scenario or --seed");
        }
        const bool geographic = scenario_path && openwar::is_geographic_scenario_file(*scenario_path);
        const auto initialize = [&] {
            if (load_path) {
                return openwar::CampaignState::load(*load_path);
            }
            if (geographic) {
                const auto seed = seed_override.value_or(0);
                if (seed > UINT32_MAX)
                    throw std::out_of_range("Seed must fit in 32 bits");
                return openwar::CampaignState(openwar::read_geographic_scenario(*scenario_path, faction),
                    static_cast<std::uint32_t>(seed));
            }
            auto settings = scenario_path ? openwar::read_procedural_settings(*scenario_path) : openwar::ProceduralSettings{};
            settings.seed = seed_override.value_or(settings.seed);
            if (demo != Demo::procedural && settings.seed > UINT32_MAX) {
                throw std::out_of_range("Seed must fit in 32 bits");
            }
            return make_campaign(demo.value_or(Demo::economy), settings.seed, faction, settings);
        };
        auto campaign = initialize();
        const bool generated = campaign.generator_version() != 0;
        const auto* map = campaign.geographic_world();
        const bool imported = map && !map->provenance.empty();
        const bool campaign_view = (generated || imported || campaign.strategic_command_enabled()) && !faction;
        const bool operations_view = campaign.has_operational_view();
        if ((faction || !commands.empty()) &&
            (!operations_view || (!load_path && !geographic && demo != Demo::operations && demo != Demo::combat))) {
            throw std::invalid_argument("Faction controls require a geographic scenario, operational checkpoint, or an operations or combat demo");
        }
        if (!commands.empty() && !faction) {
            throw std::invalid_argument("Orders require --faction WEST or EAST");
        }
        if (imported) {
            std::cout << "Imported geographic world: " << campaign.world().width() << 'x' << campaign.world().height()
                      << " sectors, " << map->roads.size() << " road segments\n";
        }
        if (generated) {
            report_procedural_world(campaign, ticks == 0);
        }
        openwar::CommandSession operator_session(campaign, faction);
        for (const auto& [command, value] : commands) {
            if (command != "--cancel") {
                apply_order(operator_session, value, command == "--ground-order");
            } else {
                const auto id = parse_number(value, "--cancel");
                if (id > UINT32_MAX) {
                    throw std::out_of_range("Order ID must fit in 32 bits");
                }
                const auto result = operator_session.cancel(*faction, openwar::MissionId{static_cast<std::uint32_t>(id)});
                if (!result.cancelled) {
                    throw std::invalid_argument(std::string(result.reason));
                }
            }
        }
        const auto perspective = operations_view || campaign_view ?
            std::optional{faction.value_or(campaign.player_faction().value_or(openwar::FactionId::neutral))} :
            std::nullopt;
        if (!ticks || speed || legacy_realtime) {
            openwar::cli::run_realtime(campaign, speed.value_or(60), ticks, events, perspective,
                                       campaign_view);
        } else {
            campaign.advance_ticks(*ticks);
            if (campaign_view) {
                if (events) {
                    openwar::cli::print_operations_events(campaign, openwar::FactionId::neutral, true);
                }
            } else if (perspective) {
                openwar::cli::print_operations(campaign, *perspective, events);
            } else {
                if (events) {
                    openwar::cli::print_events(campaign);
                }
                report(campaign, campaign.seed());
            }
        }
        if (campaign_view) {
            openwar::cli::print_campaign_summary(std::cout, campaign, campaign.seed());
        }
        if (save_path) {
            campaign.save(*save_path);
        }
    } catch (const std::exception& error) {
        std::cerr << "openwar: " << error.what() << '\n' << usage;
        return 1;
    }
}
