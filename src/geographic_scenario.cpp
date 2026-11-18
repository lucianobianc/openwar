#include "scenario.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
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

GeographicWorld read_map(GeographicFile& file, int expected_version)
{
    file.expect("OWGEO");
    const auto version = file.number(1, 65535);
    if (version != expected_version) {
        if (version == 2 && expected_version == 1)
            file.fail("OWGEO 2 is a geographic world; use an .owscenario file to start a campaign");
        if (version == 1 && expected_version == 2)
            file.fail("expected an OWGEO 2 geographic world; OWGEO 1 is a combined demonstration");
        file.fail("unsupported geographic schema " + std::to_string(version));
    }
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
    std::vector<SectorProperties> properties;
    properties.reserve(covers.size());
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
    const auto nodes = file.number<std::uint32_t>(version == 1 ? 1 : 0, 250'000);
    for (std::uint32_t id = 1; id <= nodes; ++id) {
        const auto source = file.number<std::uint64_t>(1, UINT64_MAX);
        if (!map.nodes.empty() && source <= map.nodes.back().osm_node_id)
            file.fail("road node source IDs must be unique and sorted");
        map.nodes.push_back({id, read_point(file, map), source});
    }

    file.expect("roads");
    const auto roads = file.number<std::uint32_t>(version == 1 ? 1 : 0, 500'000);
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
    WorldDefinition grid(map.columns, map.rows, map.sector_mm / 1000, std::move(properties));
    return {std::move(grid), std::move(map)};
}

geography::Profile valley_mobility()
{
    return {{geography::Rate{5000, 500}, geography::Rate{2500, 1000}, {}},
            {geography::Rate{8000, 500}, geography::Rate{6000, 750}}};
}

ScenarioDefinition read_combined_demo(const std::filesystem::path& path, std::optional<FactionId> player)
{
    GeographicFile file(path);
    auto [grid, map] = read_map(file, 1);
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
    scenario.world = std::move(grid);
    scenario.initial_sectors.clear();
    scenario.public_sectors.clear();
    scenario.objectives.clear();
    scenario.victory_objectives.clear();
    scenario.command_objectives.clear();
    scenario.operations.planner_interval_ticks = 0;
    scenario.combat.time_limit_seconds = 24 * 3600;
    scenario.stockpiles.clear();
    scenario.installations.clear();
    const auto mobility = valley_mobility();
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

class ScenarioLine {
public:
    ScenarioLine(std::string text, const std::filesystem::path& path, std::size_t line)
        : input_(std::move(text)), location_(path.string() + ":" + std::to_string(line) + ": ")
    {
    }

    std::string keyword()
    {
        input_ >> std::ws;
        if (input_.eof() || input_.peek() == '#')
            return {};
        return word();
    }

    std::string word()
    {
        std::string value;
        if (!(input_ >> value) || value.front() == '#')
            fail("missing field");
        return value;
    }

    std::string quoted()
    {
        input_ >> std::ws;
        std::string value;
        if (input_.peek() != '"' || !(input_ >> std::quoted(value)) || value.empty())
            fail("expected a nonempty quoted string");
        return value;
    }

    template<class T = std::uint32_t>
    T number(T low = 0, T high = std::numeric_limits<T>::max())
    {
        const auto token = word();
        T value{};
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size() || value < low || value > high)
            fail("invalid integer '" + token + "'");
        return value;
    }

    FactionId faction(bool allow_neutral = false)
    {
        const auto token = word();
        if (token == "WEST")
            return FactionId::west;
        if (token == "EAST")
            return FactionId::east;
        if (allow_neutral && token == "NEUTRAL")
            return FactionId::neutral;
        fail("invalid faction '" + token + "'");
    }

    bool flag() { return number(0u, 1u) != 0; }

    ResourceAmounts amounts()
    {
        ResourceAmounts values{};
        for (auto& value : values)
            value = number<std::uint64_t>();
        return values;
    }

    Resource resource()
    {
        constexpr std::array names{"oil_reserve", "crude_oil", "fuel", "materials",
                                   "spare_parts", "supplies", "ammunition"};
        const auto token = word();
        const auto found = std::ranges::find(names, token);
        if (found == names.end())
            fail("unknown resource '" + token + "'");
        return static_cast<Resource>(found - names.begin());
    }

    EquipmentCategory category()
    {
        constexpr std::array names{"helicopter", "tank", "transport", "air_defense_vehicle", "air_defense_team"};
        const auto token = word();
        const auto found = std::ranges::find(names, token);
        if (found == names.end())
            fail("unknown equipment category '" + token + "'");
        return static_cast<EquipmentCategory>(found - names.begin());
    }

    std::optional<geography::Profile> mobility()
    {
        const auto token = word();
        if (token == "default")
            return {};
        if (token == "valley")
            return valley_mobility();
        fail("unknown mobility profile '" + token + "'");
    }

    void finish()
    {
        if (!keyword().empty())
            fail("unexpected extra field");
    }

    const std::string& location() const { return location_; }

    [[noreturn]] void fail(std::string_view message) const
    {
        throw std::runtime_error(location_ + std::string(message));
    }

private:
    std::istringstream input_;
    std::string location_;
};

template<class Entries, class Id>
auto& require_reference(Entries& entries, Id id, ScenarioLine& line, std::string_view kind)
{
    const auto found = std::ranges::find(entries, id, &Entries::value_type::id);
    if (found == entries.end())
        line.fail("unknown " + std::string(kind) + " #" + std::to_string(static_cast<std::uint32_t>(id)));
    return *found;
}

ScenarioDefinition read_military_scenario(const std::filesystem::path& path, std::optional<FactionId> player)
{
    std::ifstream input(path);
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    if (!input || size_error || size > 1024 * 1024)
        throw std::runtime_error(path.string() + ": cannot read scenario or file exceeds 1 MiB");

    std::optional<ScenarioDefinition> scenario;
    std::map<std::string, Point> points;
    std::map<StockpileId, std::string> unassigned_mobile_stock;
    std::set<SectorId> control;
    std::set<std::string> settings;
    bool header = false;
    std::string text;
    std::size_t line_number = 0;
    while (std::getline(input, text)) {
        ScenarioLine line(text, path, ++line_number);
        const auto kind = line.keyword();
        if (kind.empty())
            continue;
        if (!header) {
            if (kind != "OWSCENARIO" || line.number() != 1)
                line.fail("expected OWSCENARIO 1");
            line.finish();
            header = true;
            continue;
        }
        if (!scenario) {
            if (kind != "world")
                line.fail("expected a world reference after OWSCENARIO 1");
            const auto world_path = path.parent_path() / line.quoted();
            line.finish();
            GeographicWorld world = [&] {
                try {
                    return read_geographic_world(world_path);
                } catch (const std::exception& error) {
                    line.fail("cannot load scenario world: " + std::string(error.what()));
                }
            }();
            scenario.emplace(ScenarioDefinition{std::move(world.grid), {}});
            scenario->geographic_world = std::move(world.map);
            scenario->operations.enabled = true;
            scenario->operations.player = player;
            scenario->combat.enabled = true;
            continue;
        }

        auto& s = *scenario;
        const auto& map = *s.geographic_world;
        const auto sector_at = [&](Point point) { return static_cast<SectorId>(*map.sector(point)); };
        const auto read_sector = [&] {
            return static_cast<SectorId>(line.number(0u, static_cast<std::uint32_t>(s.world.sectors().size() - 1)));
        };
        const auto find_point = [&](const std::string& name) {
            const auto point = points.find(name);
            if (point == points.end())
                line.fail("unknown geographic point '" + name + "'");
            return point->second;
        };
        if (kind == "timing" || kind == "operations" || kind == "combat") {
            if (!settings.insert(kind).second)
                line.fail("duplicate " + kind + " settings");
        }

        if (kind == "timing") {
            s.tick_seconds = line.number(1u);
            s.economy_interval_ticks = line.number(1u);
            s.loading_ticks = line.number(1u);
            s.unloading_ticks = line.number(1u);
            if (const auto limit = line.number<std::uint64_t>(); limit)
                s.combat.time_limit_seconds = limit;
        } else if (kind == "operations") {
            auto& op = s.operations;
            op.planner_interval_ticks = line.number();
            op.strategy_review_ticks = line.number();
            op.max_autonomous_missions = line.number(1u);
            op.territory_half_life_seconds = line.number(1u);
            op.contact_half_life_seconds = line.number(1u);
            op.installation_half_life_seconds = line.number(1u);
            if (op.strategy_review_ticks && !op.planner_interval_ticks)
                line.fail("strategic review requires a positive planning interval");
        } else if (kind == "combat") {
            auto& combat = s.combat;
            combat.interval_ticks = line.number(1u);
            combat.occupation_seconds = line.number<std::uint64_t>(1);
            combat.critical_occupation_seconds = line.number<std::uint64_t>(1);
            combat.stabilization_seconds = line.number<std::uint64_t>();
            combat.capitulation_seconds = line.number<std::uint64_t>(1);
            combat.reinforcement_horizon_seconds = line.number<std::uint64_t>();
        } else if (kind == "point") {
            const auto name = line.word();
            const Point point{line.number<std::int64_t>(), line.number<std::int64_t>(),
                              line.number<std::int64_t>(-20'000'000, 30'000'000)};
            if (!map.valid_position(point))
                line.fail("geographic point '" + name + "' is outside the world");
            if (name == "-" || !points.emplace(name, point).second)
                line.fail("duplicate or reserved point name '" + name + "'");
        } else if (kind == "control") {
            const auto sector = read_sector();
            const auto owner = line.faction(true);
            if (!control.insert(sector).second)
                line.fail("duplicate initial sector control");
            s.initial_sectors.push_back({sector, {owner, line.number()}});
        } else if (kind == "public") {
            s.public_sectors.push_back(read_sector());
        } else if (kind == "objective") {
            const auto faction = line.faction();
            const auto point = find_point(line.word());
            const auto sector = sector_at(point);
            const auto [target, inserted] = s.ground_destinations.try_emplace(sector, point);
            if (!inserted && target->second != point)
                line.fail("conflicting geographic objectives in one sector");
            s.objectives.push_back({faction, sector, line.number(1u)});
        } else if (kind == "victory") {
            const auto faction = line.faction();
            const auto hold_seconds = line.number<std::uint64_t>(1);
            s.victory_objectives.push_back({faction, {read_sector()}, hold_seconds});
        } else if (kind == "command_site") {
            s.command_objectives.push_back({line.faction(), {read_sector()}});
        } else if (kind == "stock") {
            Stockpile stock{StockpileId{line.number(1u)}, {}, line.faction(true)};
            const auto at = line.word();
            if (std::ranges::find(s.stockpiles, stock.id, &Stockpile::id) != s.stockpiles.end())
                line.fail("duplicate stockpile ID");
            if (at == "-") {
                unassigned_mobile_stock.emplace(stock.id, line.location());
            } else {
                stock.access_point = find_point(at);
                stock.sector = sector_at(*stock.access_point);
            }
            stock.capacity = line.amounts();
            stock.quantity = line.amounts();
            s.stockpiles.push_back(stock);
        } else if (kind == "installation") {
            const auto id = InstallationId{line.number(1u)};
            const auto stock_id = StockpileId{line.number(1u)};
            const auto& stock = require_reference(s.stockpiles, stock_id, line, "stockpile");
            if (!stock.access_point)
                line.fail("installation requires a fixed stockpile");
            const auto workers = line.number();
            const auto powered = line.flag();
            const auto integrity = line.number(0u, 100u);
            const auto interval = line.number(1u);
            const auto required = line.number();
            const auto needs_power = line.flag();
            const ProductionRecipe recipe{line.amounts(), line.amounts(), interval, required, needs_power};
            s.installations.push_back({id, stock_id, line.quoted(), stock.owner, recipe, workers, powered, integrity});
        } else if (kind == "facility") {
            const auto id = FacilityId{line.number(1u)};
            const auto stock_id = StockpileId{line.number(1u)};
            if (!require_reference(s.stockpiles, stock_id, line, "stockpile").access_point)
                line.fail("facility requires a fixed stockpile");
            s.facilities.push_back({id, stock_id, line.number(1u), line.number(), line.flag()});
        } else if (kind == "type") {
            const auto id = EquipmentTypeId{line.number(1u)};
            const auto faction = line.faction();
            const auto category = line.category();
            const auto power = line.number(0u, 1000u);
            const auto occupies = line.flag();
            const auto speed = line.number();
            const auto range = line.number();
            const auto service_interval = line.number(1u);
            ServiceProfile repair{line.number(1u)};
            repair.costs[static_cast<std::size_t>(Resource::spare_parts)] = line.number<std::uint64_t>();
            const auto mobility = line.mobility();
            s.equipment_types.push_back({id, line.quoted(), faction, category, {}, {}, {}, repair,
                service_interval, range, power, occupies, speed, mobility});
        } else if (kind == "unit") {
            const auto id = UnitId{line.number(1u)};
            const auto faction = line.faction();
            const auto point = find_point(line.word());
            const auto stock_id = StockpileId{line.number()};
            const auto facility_id = FacilityId{line.number()};
            const auto morale = line.number(0u, 100u);
            const auto sector = sector_at(point);
            MilitaryUnit unit{id, line.quoted(), faction, sector};
            if (facility_id != FacilityId{}) {
                const auto& facility = require_reference(s.facilities, facility_id, line, "facility");
                const auto& stores = require_reference(s.stockpiles, facility.stockpile, line, "stockpile");
                if (stores.owner != faction || stores.sector != sector)
                    line.fail("unit facility must be in its deployment sector and belong to its faction");
                if (stock_id == StockpileId{} && stores.access_point != point)
                    line.fail("unit without mobile stock must deploy at its facility point");
                unit.facility = facility_id;
            }
            if (stock_id != StockpileId{}) {
                auto& stock = require_reference(s.stockpiles, stock_id, line, "stockpile");
                if (stock.owner != faction || !unassigned_mobile_stock.erase(stock_id))
                    line.fail("unit needs its own same-faction mobile stockpile");
                stock.sector = sector;
                CombatUnit combat{id, stock_id, morale};
                combat.position = point;
                s.combat_units.push_back(combat);
            } else if (!unit.facility) {
                line.fail("unit needs either mobile stock or a facility");
            }
            s.units.push_back(std::move(unit));
        } else if (kind == "equipment") {
            const auto id = EquipmentId{line.number(1u)};
            const auto type = EquipmentTypeId{line.number(1u)};
            const auto unit = UnitId{line.number(1u)};
            const auto& profile = require_reference(s.equipment_types, type, line, "equipment type");
            if (profile.faction != require_reference(s.units, unit, line, "unit").faction)
                line.fail("equipment and unit belong to different factions");
            s.equipment.push_back({id, type, unit});
        } else if (kind == "carrier") {
            const auto id = CarrierId{line.number(1u)};
            const auto faction = line.faction();
            const auto point = find_point(line.word());
            Carrier carrier{id, faction, sector_at(point), line.number<std::uint64_t>(1), line.number<std::uint64_t>()};
            carrier.position = point;
            carrier.mobility = line.mobility();
            s.carriers.push_back(carrier);
        } else if (kind == "shipment" || kind == "resupply") {
            const auto id = kind == "shipment" ? ShipmentId{line.number(1u)} : ShipmentId{};
            const auto origin = StockpileId{line.number(1u)};
            const auto destination = StockpileId{line.number(1u)};
            const auto& from = require_reference(s.stockpiles, origin, line, "stockpile");
            const auto& to = require_reference(s.stockpiles, destination, line, "stockpile");
            if (origin == destination || from.owner != to.owner || from.owner == FactionId::neutral)
                line.fail("supply endpoints must be distinct stockpiles of one military faction");
            const auto resource = line.resource();
            const auto amount = line.number<std::uint64_t>(1);
            if (kind == "shipment") {
                s.initial_supply_requests.push_back({id, origin, destination, resource, amount,
                    line.number<int>(std::numeric_limits<int>::min())});
            } else {
                s.resupply.push_back({origin, destination, resource, amount, line.number<std::uint64_t>(amount)});
            }
        } else {
            line.fail("unknown record '" + kind + "'");
        }
        line.finish();
    }
    if (input.bad())
        throw std::runtime_error(path.string() + ": failed while reading scenario");
    if (!scenario)
        throw std::runtime_error(path.string() + ": missing OWSCENARIO 1 header or world reference");
    if (!unassigned_mobile_stock.empty()) {
        const auto& [id, location] = *unassigned_mobile_stock.begin();
        throw std::runtime_error(location + "mobile stockpile #" +
            std::to_string(static_cast<std::uint32_t>(id)) + " is not assigned to a unit");
    }
    return std::move(*scenario);
}

}  // namespace

GeographicWorld read_geographic_world(const std::filesystem::path& path)
{
    GeographicFile file(path);
    auto world = read_map(file, 2);
    file.expect("end");
    file.finish();
    return world;
}

bool is_geographic_scenario_file(const std::filesystem::path& path)
{
    return path.extension() == ".owscenario" || path.extension() == ".owgeo";
}

ScenarioDefinition read_geographic_scenario(const std::filesystem::path& path, std::optional<FactionId> player)
{
    if (path.extension() == ".owscenario")
        return read_military_scenario(path, player);
    return read_combined_demo(path, player);
}

}  // namespace openwar
