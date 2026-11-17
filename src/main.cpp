#include "campaign.hpp"
#include "realtime.hpp"

#include <charconv>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view usage = R"(Usage: openwar [--demo NAME] [--speed N] [--ticks N] [--events] [--seed N]

Without a tick limit, the dashboard runs in real time until Ctrl+C.

Options
  --demo NAME  Select economy (default), logistics, maintenance, or operations.
  --speed N    Pace the run at N simulated seconds per real second (default 60).
  --ticks N    Stop after N ticks. Runs in batch mode unless speed is specified.
  --events     Show timestamped campaign events.
  --seed N     Set the deterministic seed (default 0).
  --faction WEST|EAST  Use one faction's view in the operations demo.
  --order TYPE:SECTOR  Queue a recon, patrol, defend, or reposition order.
  --cancel ID         Cancel a scheduled order before the simulation starts.
  --help       Show this help.

Examples
  openwar
  openwar --speed 600
  openwar --demo logistics --ticks 100
  openwar --demo maintenance --speed 600 --ticks 12 --events
  openwar --demo operations --faction WEST --order recon:2 --ticks 12 --events
)";

enum class Demo { economy, logistics, maintenance, operations };

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
    throw std::invalid_argument("Unknown demonstration: " + std::string(name));
}

openwar::CampaignState make_campaign(Demo demo, std::uint32_t seed, std::optional<openwar::FactionId> faction)
{
    using namespace openwar;
    switch (demo) {
    case Demo::economy:
        return CampaignState(example_scenario(), seed);
    case Demo::maintenance:
        return maintenance_example_campaign(seed);
    case Demo::operations:
        return operations_example_campaign(seed, faction);
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

void apply_order(openwar::CampaignState& campaign, openwar::FactionId faction, std::string_view order)
{
    using namespace openwar;
    const auto separator = order.find(':');
    if (separator == std::string_view::npos) {
        throw std::invalid_argument("Expected order TYPE:SECTOR");
    }
    const auto sector = parse_number(order.substr(separator + 1), "--order sector");
    if (sector > UINT32_MAX) {
        throw std::out_of_range("Order sector must fit in 32 bits");
    }
    for (const auto kind : {MissionKind::recon, MissionKind::patrol, MissionKind::defend, MissionKind::reposition}) {
        if (order.substr(0, separator) == mission_name(kind)) {
            const auto result = campaign.issue_order(faction, kind, SectorId{static_cast<std::uint32_t>(sector)});
            if (!result.mission) {
                throw std::invalid_argument(std::string(result.reason));
            }
            return;
        }
    }
    throw std::invalid_argument("Unknown order type");
}

void report(const openwar::CampaignState& campaign, std::uint32_t seed)
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

}  // namespace

int main(int argc, char* argv[])
{
    std::uint32_t seed = 0;
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
                option != "--faction" && option != "--order" && option != "--cancel") {
                throw std::invalid_argument("Unknown option: " + std::string(option));
            }
            if (++i == argc) {
                throw std::invalid_argument("Missing value for " + std::string(option));
            }
            if (option == "--demo") {
                select_demo(parse_demo(argv[i]));
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
            if (option == "--order" || option == "--cancel") {
                commands.emplace_back(option, argv[i]);
                continue;
            }
            const auto value = parse_number(argv[i], option);
            if (option == "--seed") {
                if (value > UINT32_MAX) {
                    throw std::out_of_range("Seed must fit in 32 bits");
                }
                seed = static_cast<std::uint32_t>(value);
            } else if (option == "--ticks") {
                ticks = value;
            } else {
                if (value == 0 || value > UINT32_MAX) {
                    throw std::out_of_range("Speed must be a positive 32-bit integer");
                }
                speed = static_cast<std::uint32_t>(value);
            }
        }
        if ((faction || !commands.empty()) && demo != Demo::operations) {
            throw std::invalid_argument("Faction controls require --demo operations");
        }
        if (!commands.empty() && !faction) {
            throw std::invalid_argument("Orders require --faction WEST or EAST");
        }
        auto campaign = make_campaign(demo.value_or(Demo::economy), seed, faction);
        for (const auto& [command, value] : commands) {
            if (command == "--order") {
                apply_order(campaign, *faction, value);
            } else {
                const auto id = parse_number(value, "--cancel");
                if (id > UINT32_MAX || !campaign.cancel_order(*faction, openwar::MissionId{static_cast<std::uint32_t>(id)})) {
                    throw std::invalid_argument("No cancellable order with that ID");
                }
            }
        }
        const auto perspective = demo == Demo::operations ?
            std::optional{faction.value_or(openwar::FactionId::neutral)} : std::nullopt;
        if (!ticks || speed || legacy_realtime) {
            openwar::cli::run_realtime(campaign, speed.value_or(60), ticks, events, perspective);
        } else {
            campaign.advance_ticks(*ticks);
            if (perspective) {
                openwar::cli::print_operations(campaign, *perspective, events);
            } else {
                if (events) {
                    openwar::cli::print_events(campaign);
                }
                report(campaign, seed);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "openwar: " << error.what() << '\n' << usage;
        return 1;
    }
}
