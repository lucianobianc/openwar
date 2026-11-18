#include "scenario.hpp"

#include <charconv>
#include <fstream>
#include <iomanip>
#include <set>
#include <stdexcept>

namespace openwar {
namespace {

using geography::Point;

class GeographicFile {
public:
    explicit GeographicFile(const std::filesystem::path& path) : input_(path), path_(path.string())
    {
        if (!input_ || std::filesystem::file_size(path) > 64 * 1024 * 1024)
            fail("cannot read geographic file or file exceeds 64 MiB");
    }

    void expect(std::string_view expected)
    {
        if (token() != expected)
            fail("expected " + std::string(expected));
    }

    template<class T>
    T number(T low, T high)
    {
        const auto text = token();
        T value{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
            value < low || value > high)
            fail("invalid geographic integer " + text.substr(0, 80));
        return value;
    }

    std::string quoted(std::size_t limit)
    {
        std::string content;
        input_ >> std::ws;
        if (input_.peek() != '"' || !(input_ >> std::quoted(content)) || content.size() > limit)
            fail("invalid or oversized geographic string");
        return content;
    }

    void finish()
    {
        input_ >> std::ws;
        if (!input_.eof())
            fail("unexpected data after end marker");
    }

    [[noreturn]] void fail(std::string_view message) const
    {
        throw std::runtime_error(path_ + ": " + std::string(message));
    }

private:
    std::string token()
    {
        std::string text;
        if (!(input_ >> std::setw(256) >> text) || text.size() == 256)
            fail("missing or oversized geographic token");
        return text;
    }

    std::ifstream input_;
    std::string path_;
};

Point read_point(GeographicFile& file, const geography::Map& map)
{
    return {file.number<std::int64_t>(0, map.columns * map.sector_mm - 1),
        file.number<std::int64_t>(0, map.rows * map.sector_mm - 1),
        file.number<std::int64_t>(-20'000'000, 30'000'000)};
}

geography::Map read_map(GeographicFile& file, std::vector<SectorProperties>& properties)
{
    file.expect("OWGEO");
    if (file.number(1, 65535) != 1)
        file.fail("unsupported geographic schema");
    file.expect("grid");
    geography::Map map{};
    map.columns = file.number(1, 128);
    map.rows = file.number(1, 128);
    map.sector_mm = file.number<std::int64_t>(1000, 100'000'000);
    map.tile_mm = file.number<std::int64_t>(1000, map.sector_mm);
    if (map.sector_mm % 1000 != 0 || map.sector_mm % map.tile_mm != 0)
        file.fail("tile size must divide sector size");
    file.expect("provenance");
    map.provenance = file.quoted(512 * 1024);
    if (map.provenance.empty())
        file.fail("missing geographic provenance");

    const auto tiles_per_sector = map.sector_mm / map.tile_mm;
    const auto columns = map.columns * tiles_per_sector;
    const auto rows = map.rows * tiles_per_sector;
    if (columns * rows > 2'000'000)
        file.fail("terrain grid exceeds 2 million tiles");
    file.expect("tiles");
    std::vector<std::array<std::size_t, 4>> covers(map.columns * map.rows);
    for (std::int64_t y = 0; y < rows; ++y) {
        for (std::int64_t x = 0; x < columns; ++x) {
            const auto cover = file.number(0, 3);
            const auto elevation = file.number<std::int64_t>(-20'000'000, 30'000'000);
            if (cover != 0 || elevation != 0)
                map.tiles.emplace(std::pair{x, y}, geography::Tile{static_cast<geography::Cover>(cover), elevation});
            ++covers[(y / tiles_per_sector) * map.columns + x / tiles_per_sector][cover];
        }
    }
    for (const auto& count : covers) {
        const auto land = count[0] + count[1] + count[3];
        const auto terrain = !land ? Terrain::water : count[1] > land / 2 ? Terrain::forest : Terrain::plains;
        properties.push_back({terrain, land != 0});
    }

    file.expect("waterways");
    for (auto count = file.number(0, 100'000); count; --count) {
        const auto from = read_point(file, map);
        const auto to = read_point(file, map);
        if (from.elevation || to.elevation || from == to)
            file.fail("invalid waterway line");
        map.waterways.push_back({from, to});
    }

    file.expect("nodes");
    const auto nodes = file.number<std::uint32_t>(1, 250'000);
    for (std::uint32_t id = 1; id <= nodes; ++id) {
        const auto source = file.number<std::uint64_t>(1, UINT64_MAX);
        if (!map.nodes.empty() && source <= map.nodes.back().osm_node_id)
            file.fail("road node source IDs must be unique and sorted");
        map.nodes.push_back({id, read_point(file, map), source});
    }

    file.expect("roads");
    const auto roads = file.number<std::uint32_t>(1, 500'000);
    for (std::uint32_t id = 1; id <= roads; ++id) {
        const auto source = file.number<std::uint64_t>(1, UINT64_MAX);
        const auto from = file.number(1u, nodes);
        const auto to = file.number(1u, nodes);
        const auto surface = static_cast<geography::Surface>(file.number(0, 1));
        const auto direction = static_cast<geography::Direction>(file.number(0, 2));
        const auto crossing = static_cast<geography::Crossing>(file.number(0, 1));
        const auto speed = file.number(1, 50'000);
        geography::RoadSegment road{id, from, to, {}, surface, direction, crossing, speed};
        road.osm_way_id = source;
        road.interior_access = file.number(0, 1) != 0;
        for (auto count = file.number(0, 20'000); count; --count)
            road.waypoints.push_back(read_point(file, map));
        auto previous = map.nodes[from - 1].position;
        const auto valid_step = [&](Point next) {
            if (previous.east == next.east && previous.south == next.south)
                file.fail("zero-length geographic road section");
            previous = next;
        };
        for (const auto point : road.waypoints)
            valid_step(point);
        valid_step(map.nodes[to - 1].position);
        map.roads.push_back(std::move(road));
    }
    file.expect("features");
    std::optional<std::pair<geography::OsmElement, std::uint64_t>> previous;
    for (auto count = file.number(0, 250'000); count; --count) {
        const auto kind = static_cast<geography::SiteKind>(file.number(0, 8));
        const auto element = static_cast<geography::OsmElement>(file.number(0, 2));
        const auto identity = file.number<std::uint64_t>(1, UINT64_MAX);
        const auto key = std::pair{element, identity};
        if (previous && key <= *previous)
            file.fail("geographic feature IDs must be unique and sorted");
        previous = key;
        const auto point = read_point(file, map);
        const auto length = file.number<std::uint64_t>(0, 1'000'000'000);
        const auto width = file.number<std::uint64_t>(0, 1'000'000'000);
        const auto area = file.number<std::uint64_t>(0, 1'000'000'000'000);
        const auto name = file.quoted(1024);
        const auto surface = file.quoted(128);
        const auto geometry = file.quoted(512 * 1024);
        map.site_features.push_back({kind, element, identity, point, length, width, area,
            name, surface, geometry});
    }
    return map;
}

}  // namespace

ScenarioDefinition read_geographic_scenario(const std::filesystem::path& path, std::optional<FactionId> player)
{
    GeographicFile file(path);
    std::vector<SectorProperties> properties;
    auto map = read_map(file, properties);
    std::array<Point, 4> sites{};
    std::set<SectorId> site_sectors;
    const auto sector = [&](Point point) { return static_cast<SectorId>(*map.sector(point)); };
    for (auto& site : sites) {
        file.expect("site");
        site = read_point(file, map);
        if (!site_sectors.insert(sector(site)).second)
            file.fail("deployment sites must occupy distinct sectors");
    }
    file.expect("end");
    file.finish();

    auto scenario = combat_example_scenario(player);
    scenario.world = WorldDefinition(map.columns, map.rows, map.sector_mm / 1000, std::move(properties));
    scenario.initial_sectors.clear();
    scenario.public_sectors.clear();
    scenario.objectives.clear();
    scenario.victory_objectives.clear();
    scenario.command_objectives.clear();
    scenario.operations.planner_interval_ticks = 0;
    scenario.combat.time_limit_seconds = 24 * 3600;
    scenario.stockpiles.clear();
    scenario.installations.clear();
    geography::Profile mobility{};
    mobility.terrain = {geography::Rate{5000, 500}, geography::Rate{2500, 1000}, {}};
    mobility.roads = {geography::Rate{8000, 500}, geography::Rate{6000, 750}};
    std::map<SectorId, FactionId> ownership;
    for (std::uint32_t i = 0; i < 2; ++i) {
        const auto faction = i ? FactionId::east : FactionId::west;
        const auto base = sites[2 * i];
        const auto outpost = sites[2 * i + 1];
        const auto route = geography::find_route(map, mobility, base, outpost);
        if (route.status == geography::RouteStatus::limit_exceeded)
            file.fail(std::string(i ? "EAST" : "WEST") +
                " deployment route exceeded the geographic search budget (raster " +
                std::to_string(route.raster_expanded) + ", projections " +
                std::to_string(route.projection_checks) + ", road tiles " +
                std::to_string(route.road_tile_checks) + ")");
        if (route.status != geography::RouteStatus::found || route.fuel_ml > 500'000)
            file.fail(std::string(i ? "EAST" : "WEST") +
                " deployment outpost has no reachable route within the fuel budget");
        const auto assign = [&](SectorId id) {
            const auto owner = ownership.try_emplace(id, faction).first;
            if (owner->second != faction || !scenario.world.sector(id).properties.traversable)
                file.fail("deployment corridors overlap or cross a wholly submerged strategic sector");
        };
        assign(sector(base));
        auto progress = geography::begin(route, route.fuel_ml);
        geography::advance(map, route, progress, route.duration_us);
        for (const auto& crossing : progress.crossings)
            assign(static_cast<SectorId>(crossing.to));

        const auto base_id = StockpileId{i + 1};
        const auto post_id = StockpileId{i + 3};
        const auto mobile_id = StockpileId{i + 10};
        const ResourceAmounts supplies{0, 0, 500, 0, 20, 100, 40};
        scenario.stockpiles.push_back({base_id, sector(base), faction, supplies, supplies, {}, base});
        scenario.stockpiles.push_back({post_id, sector(outpost), faction, supplies, {}, {}, outpost});
        scenario.stockpiles.push_back({mobile_id, sector(base), faction, supplies, supplies});
        scenario.installations.push_back({InstallationId{i + 1}, post_id,
            i ? "East field depot" : "West field depot", faction,
            {{0, 0, 0, 0, 1}, {0, 0, 0, 0, 0, 1}}, 1, false});
        scenario.units[i].sector = sector(base);
        scenario.combat_units[i].position = base;
        scenario.equipment_types[i].mobility = mobility;
        scenario.ground_destinations.emplace(sector(outpost), outpost);
        scenario.objectives.push_back({faction, sector(outpost), 1});
        scenario.carriers.push_back({CarrierId{i + 1}, faction, sector(base), 20, 500});
        scenario.carriers.back().position = base;
        scenario.carriers.back().mobility = mobility;
        scenario.initial_supply_requests.push_back({ShipmentId{i + 1}, base_id, post_id, Resource::supplies, 12});
    }
    for (const auto& [id, faction] : ownership)
        scenario.initial_sectors.push_back({id, {faction, scenario.ground_destinations.contains(id) ? 1u : 0u}});
    scenario.geographic_world = std::move(map);
    return scenario;
}

}  // namespace openwar
