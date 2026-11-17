#include "campaign.hpp"
#include "realtime.hpp"

#include <charconv>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view usage =
    "Usage: openwar [--seed UINT32] [--ticks UINT64] [--logistics-demo]\n"
    "               [--realtime [--speed N]] [--events]\n"
    "  --realtime  Advance until Ctrl+C or the optional --ticks limit.\n"
    "  --speed N   Positive simulated seconds per real second (default: 1).\n"
    "  --events    Display timestamped economy and logistics events.\n";

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
}

}  // namespace

int main(int argc, char* argv[])
{
    std::uint32_t seed = 0;
    std::optional<std::uint64_t> ticks;
    std::optional<std::uint32_t> speed;
    bool logistics_demo = false;
    bool realtime = false;
    bool events = false;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string_view option = argv[i];
            if (option == "--help") {
                std::cout << usage;
                return 0;
            }
            if (option == "--logistics-demo") {
                logistics_demo = true;
                continue;
            }
            if (option == "--realtime") {
                realtime = true;
                continue;
            }
            if (option == "--events") {
                events = true;
                continue;
            }
            if (option != "--seed" && option != "--ticks" && option != "--speed") {
                throw std::invalid_argument("Unknown option: " + std::string(option));
            }
            if (++i == argc) {
                throw std::invalid_argument("Missing value for " + std::string(option));
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
        if (speed && !realtime) {
            throw std::invalid_argument("--speed requires --realtime");
        }

        using namespace openwar;
        CampaignState campaign(logistics_demo ? logistics_example_scenario() : example_scenario(), seed);
        if (logistics_demo) {
            campaign.request_supply({ShipmentId{1}, StockpileId{1}, StockpileId{3}, Resource::fuel, 80});
        }
        if (realtime) {
            cli::run_realtime(campaign, speed.value_or(1), ticks, events);
        } else {
            campaign.advance_ticks(ticks.value_or(1));
            if (events) {
                cli::print_events(campaign);
            }
            report(campaign, seed);
        }
    } catch (const std::exception& error) {
        std::cerr << "openwar: " << error.what() << '\n' << usage;
        return 1;
    }
}
