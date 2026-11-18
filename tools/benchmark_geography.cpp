#include "scenario.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv)
{
    using namespace openwar;
    using namespace openwar::geography;
    using Clock = std::chrono::steady_clock;
    if (argc != 2) {
        std::cerr << "Usage: benchmark-geography SCENARIO.owgeo\n";
        return 2;
    }
    try {
        const auto start = Clock::now();
        auto scenario = read_geographic_scenario(argv[1], FactionId::west);
        const auto& map = *scenario.geographic_world;
        std::cout << "Load and validate: "
                  << std::chrono::duration<double, std::milli>(Clock::now() - start).count() << " ms\n";
        std::cout << "Catalog: " << map.site_features.size() << " features, "
                  << map.site_candidates(SiteKind::helipad).size() << " helipads\n";

        const auto access_point = [&](StockpileId id) {
            return *std::ranges::find(scenario.stockpiles, id, &Stockpile::id)->access_point;
        };
        const auto status = [](RouteStatus result) {
            switch (result) {
            case RouteStatus::found: return "found";
            case RouteStatus::unreachable: return "unreachable";
            case RouteStatus::limit_exceeded: return "search limit";
            }
            return "unknown";
        };
        const auto& mobility = *scenario.carriers.front().mobility;
        const auto measure = [&](std::string_view label, Point a, Point b, const Profile& profile) {
            const auto begin = Clock::now();
            Route route;
            for (int i = 0; i < 3; ++i)
                route = find_route(map, profile, a, b);
            const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - begin).count() / 3;
            std::int64_t millimetres = 0;
            std::size_t road_legs = 0, bridge_legs = 0;
            for (const auto& leg : route.legs) {
                millimetres += leg.distance_mm;
                if (leg.road) {
                    ++road_legs;
                    bridge_legs += map.roads.at(*leg.road - 1).crossing == Crossing::bridge;
                }
            }
            std::cout << label << ": " << status(route.status) << ", " << elapsed << " ms, "
                      << millimetres / 1000 << " m, " << road_legs << " road legs, "
                      << bridge_legs << " bridge legs, " << route.legs.size() - road_legs
                      << " terrain legs, " << route.raster_expanded << " raster vertices\n";
            if (route.status == RouteStatus::limit_exceeded) {
                const auto side = map.sector_mm / map.tile_mm;
                if (map.columns * map.rows * side * side <= 40'000) {
                    const QueryLimits reference{std::max(map.columns, map.rows) * side,
                        40'000, 2'000'000, 2'000'000, 5'000'000};
                    const auto fine = find_route(map, profile, a, b, reference);
                    std::cout << "  Full fine-grid comparison: " << status(fine.status) << '\n';
                }
            }
        };
        measure("WEST outbound", access_point(StockpileId{1}), access_point(StockpileId{3}), mobility);
        measure("WEST return", access_point(StockpileId{3}), access_point(StockpileId{1}), mobility);
        measure("EAST outbound", access_point(StockpileId{2}), access_point(StockpileId{4}), mobility);
        measure("Across the deployment areas", access_point(StockpileId{1}), access_point(StockpileId{4}), mobility);
        auto offroad = mobility;
        offroad.roads = {};
        measure("WEST without roads", access_point(StockpileId{1}), access_point(StockpileId{3}), offroad);

        const auto west_outpost = access_point(StockpileId{3});
        const auto target = static_cast<SectorId>(*map.sector(west_outpost));
        CampaignState campaign(std::move(scenario), 42);
        const auto order = campaign.issue_order(FactionId::west, MissionKind::defend, target);
        if (!order.mission)
            throw std::runtime_error(std::string(order.reason));
        campaign.advance_ticks(120);
        if (campaign.combat_units().at(UnitId{1}).position != west_outpost ||
            campaign.shipments().at(ShipmentId{1}).status != ShipmentStatus::delivered ||
            campaign.carriers().at(CarrierId{1}).position != west_outpost ||
            campaign.stockpile_position(StockpileId{3}) != west_outpost ||
            campaign.shipments().at(ShipmentId{2}).status != ShipmentStatus::delivered)
            throw std::runtime_error("Geographic campaign did not finish the movement and deliveries");
        std::cout << "Ground formation and supply carrier reached the exact WEST outpost; both deliveries completed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
