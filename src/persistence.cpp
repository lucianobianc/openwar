#include "campaign.hpp"

#include <algorithm>
#include <bit>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace openwar {
namespace {

constexpr std::string_view signature = "OPENWAR-CHECKPOINT\n";
// Change the schema version for layout changes and the rules version for changes to simulation outcomes.
constexpr std::uint64_t schema_version = 4;
constexpr std::uint64_t rules_version = 5;
constexpr std::size_t header_size = signature.size() + 16;
constexpr std::size_t checksum_size = 8;

[[noreturn]] void invalid_checkpoint()
{
    throw std::runtime_error("Invalid campaign checkpoint");
}

std::uint64_t fingerprint(std::string_view bytes)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : bytes) {
        hash = (hash ^ byte) * 1099511628211ull;
    }
    return hash;
}

// Field order belongs to the schema. Never serialize object memory, padding, or pointers.
template<class Archive, class T>
using Field = std::conditional_t<Archive::reading, T, const T>;

template<class A> void fields(A& a, Field<A, SectorProperties>& v)
{ a(v.terrain, v.traversable); }

template<class A> void fields(A& a, Field<A, SectorState>& v)
{ a(v.controller, v.objective_points, v.control, v.occupier, v.occupation_since, v.last_change_tick); }

template<class A> void fields(A& a, Field<A, InitialSectorState>& v)
{ a(v.sector, v.state); }

template<class A> void fields(A& a, Field<A, Stockpile>& v)
{
    a(v.id, v.sector, v.owner, v.capacity, v.quantity, v.reserved);
    if (a.version >= 3) {
        a(v.access_point);
    }
}

template<class A> void fields(A& a, Field<A, ProductionRecipe>& v)
{ a(v.inputs, v.outputs, v.intervals_per_batch, v.workers_required, v.requires_power); }

template<class A> void fields(A& a, Field<A, Installation>& v)
{ a(v.id, v.stockpile, v.name, v.operator_faction, v.recipe, v.workers, v.has_power, v.integrity); }

template<class A> void fields(A& a, Field<A, InstallationState>& v)
{ a(v.installation, v.work, v.reactivation_seconds); }

template<class A> void fields(A& a, Field<A, EconomyEntry>& v)
{ a(v.tick, v.stockpile, v.producer, v.consumed, v.produced, v.shipment, v.equipment, v.sequence); }

template<class A> void fields(A& a, Field<A, TransportRoute>& v)
{ a(v.id, v.from, v.to, v.travel_ticks, v.slots, v.fuel_cost, v.passable); }

template<class A> void fields(A& a, Field<A, Carrier>& v)
{
    a(v.id, v.faction, v.sector, v.capacity, v.fuel, v.shipment, v.lost, v.tank_capacity);
    if (a.version >= 3) {
        a(v.position, v.mobility, v.spent_ml);
    }
}

template<class A> void fields(A& a, Field<A, SupplyRequest>& v)
{ a(v.id, v.origin, v.destination, v.resource, v.quantity, v.priority, v.deadline); }

template<class A> void fields(A& a, Field<A, ResupplyPolicy>& v)
{ a(v.origin, v.destination, v.resource, v.threshold, v.target, v.priority); }

template<class A> void fields(A& a, Field<A, Shipment>& v)
{
    a(v.request, v.status, v.carrier, v.edge, v.remaining_ticks, v.cargo, v.pickup_fuel);
    if (a.version >= 3) {
        a(v.journey, v.road, v.delay);
    }
}

template<class A> void fields(A& a, Field<A, LogisticsEvent>& v)
{ a(v.tick, v.shipment, v.kind, v.quantity, v.sector, v.sequence); }

template<class A> void fields(A& a, Field<A, ServiceProfile>& v)
{ a(v.seconds, v.costs, v.technicians); }

template<class A> void fields(A& a, Field<A, geography::Point>& v)
{ a(v.east, v.south, v.elevation); }

template<class A> void fields(A& a, Field<A, geography::Tile>& v)
{ a(v.cover, v.elevation); }

template<class A> void fields(A& a, Field<A, geography::Rectangle>& v)
{ a(v.west, v.north, v.east, v.south); }

template<class A> void fields(A& a, Field<A, geography::Waterway>& v)
{ a(v.from, v.to); }

template<class A> void fields(A& a, Field<A, geography::RoadNode>& v)
{
    a(v.id, v.position);
    if (a.version >= 4) {
        a(v.osm_node_id);
    }
}

template<class A> void fields(A& a, Field<A, geography::RoadSegment>& v)
{
    a(v.id, v.from, v.to, v.waypoints, v.surface, v.direction, v.crossing, v.speed_limit_mm_s,
      v.interior_access, v.closed, v.interior_access_points);
    if (a.version >= 3) {
        a(v.slots);
    }
    if (a.version >= 4) {
        a(v.osm_way_id);
    }
}

template<class A> void fields(A& a, Field<A, geography::SiteFeature>& v)
{
    a(v.kind, v.element, v.source_id, v.position, v.length_mm, v.width_mm, v.area_m2,
      v.name, v.surface, v.geometry_wkt);
}

template<class A> void fields(A& a, Field<A, geography::Map>& v)
{
    a(v.columns, v.rows, v.sector_mm, v.tile_mm, v.default_tile, v.tiles, v.barriers, v.nodes, v.roads);
    if (a.version >= 4) {
        a(v.waterways, v.provenance, v.site_features);
    }
}

template<class A> void fields(A& a, Field<A, geography::Rate>& v)
{ a(v.speed_mm_s, v.fuel_ml_km, v.uphill_speed_mm_s, v.max_uphill_permille, v.max_downhill_permille); }

template<class A> void fields(A& a, Field<A, geography::Profile>& v)
{ a(v.terrain, v.roads); }

template<class A> void fields(A& a, Field<A, geography::Leg>& v)
{ a(v.from, v.to, v.distance_mm, v.duration_us, v.fuel_ml, v.road); }

template<class A> void fields(A& a, Field<A, geography::Route>& v)
{
    a(v.status, v.origin, v.legs, v.duration_us, v.fuel_ml,
      v.raster_expanded, v.projection_checks, v.road_tile_checks);
}

template<class A> void fields(A& a, Field<A, geography::Progress>& v)
{ a(v.position, v.leg, v.leg_elapsed_us, v.elapsed_us, v.entry_paid); }

template<class A> void fields(A& a, Field<A, TransportJourney>& v)
{ a(v.route, v.progress, v.destination); }

template<class A> void fields(A& a, Field<A, GroundJourney>& v)
{ a(v.route, v.progress, v.planned_speed_kph); }

template<class A> void fields(A& a, Field<A, EquipmentType>& v)
{
    a(v.id, v.name, v.faction, v.category, v.preparation, v.inspection, v.scheduled, v.repair,
      v.service_interval_seconds, v.operation_range, v.combat_power, v.can_occupy, v.ground_speed_kph);
    if (a.version != 1) {
        a(v.mobility);
    }
}

template<class A> void fields(A& a, Field<A, MaintenanceFacility>& v)
{ a(v.id, v.stockpile, v.bays, v.technicians, v.operational, v.supported_types); }

template<class A> void fields(A& a, Field<A, MilitaryUnit>& v)
{ a(v.id, v.name, v.faction, v.sector, v.parent, v.facility); }

template<class A> void fields(A& a, Field<A, EquipmentAssignment>& v)
{ a(v.id, v.type, v.unit, v.reserve_source, v.operating_seconds); }

template<class A> void fields(A& a, Field<A, ServiceJob>& v)
{ a(v.kind, v.requested_tick, v.work_seconds, v.started); }

template<class A> void fields(A& a, Field<A, Equipment>& v)
{
    a(v.assignment, v.status, v.operating_seconds, v.last_service_seconds, v.damage,
      v.job, v.reinforcement_eta, v.arrival_delayed, v.needs_turnaround);
}

template<class A> void fields(A& a, Field<A, ForcesEvent>& v)
{ a(v.tick, v.equipment, v.status, v.service, v.kind, v.sequence); }

template<class A> void fields(A& a, Field<A, OperationsSettings>& v)
{
    a(v.enabled, v.planner_interval_ticks, v.territory_half_life_seconds, v.contact_half_life_seconds,
      v.installation_half_life_seconds, v.player, v.max_autonomous_missions, v.strategy_review_ticks,
      v.reinforcement_travel_ticks, v.coordination_window_ticks);
}

template<class A> void fields(A& a, Field<A, StrategicPlan>& v)
{ a(v.focus, v.reviewed_tick, v.ground_losses, v.recovering); }

template<class A> void fields(A& a, Field<A, Objective>& v)
{ a(v.faction, v.sector, v.priority); }

template<class A> void fields(A& a, Field<A, SectorReport>& v)
{ a(v.sector, v.estimated_controller, v.observed_tick, v.confidence, v.internal, v.contested, v.surveyed); }

template<class A> void fields(A& a, Field<A, ContactReport>& v)
{ a(v.id, v.estimated_sector, v.classification, v.observed_tick, v.confidence); }

template<class A> void fields(A& a, Field<A, InstallationReport>& v)
{ a(v.sector, v.observed_tick, v.confidence); }

template<class A> void fields(A& a, Field<A, Mission>& v)
{
    a(v.id, v.faction, v.kind, v.target, v.equipment, v.launch_stock, v.fuel, v.duration_ticks, v.status,
      v.launch_tick, v.detections, v.manual, v.relocation, v.ground_unit, v.ground_path);
    if constexpr (A::reading) {
        if (a.version == 1) {
            std::size_t leg{};
            std::uint64_t progress{};
            a(leg, v.preparation_reserved, progress, v.earliest_launch_tick);
            if (v.ground_unit) {
                a.legacy_missions.emplace(std::pair{v.faction, v.id}, std::pair{leg, progress});
            }
            return;
        }
    }
    a(v.preparation_reserved, v.earliest_launch_tick, v.ground_destination, v.ground_journey);
}

template<class A> void fields(A& a, Field<A, OperationsEvent>& v)
{ a(v.tick, v.faction, v.sector, v.mission, v.reason, v.sequence); }

template<class A> void fields(A& a, Field<A, CombatSettings>& v)
{
    a(v.enabled, v.interval_ticks, v.occupation_seconds, v.critical_occupation_seconds,
      v.stabilization_seconds, v.capitulation_seconds, v.reinforcement_horizon_seconds, v.time_limit_seconds);
}

template<class A> void fields(A& a, Field<A, CombatUnit>& v)
{
    a(v.unit, v.stores, v.morale, v.cohesion, v.fatigue, v.experience, v.status, v.withdrawal,
      v.suppression);
    if constexpr (A::reading) {
        if (a.version == 1) {
            std::uint64_t progress{};
            a(progress);
            if (v.withdrawal) {
                a.legacy_withdrawals.emplace(v.unit, progress);
            }
            return;
        }
    }
    a(v.position, v.withdrawal_journey, v.movement_fuel_credit_ml);
}

template<class A> void fields(A& a, Field<A, BattleEvent>& v)
{
    a(v.tick, v.sector, v.reason, v.unit, v.amount, v.sequence, v.previous_controller);
    if (a.version != 1) {
        a(v.movement_offset_us);
    }
}

template<class A> void fields(A& a, Field<A, VictoryObjective>& v)
{ a(v.faction, v.sectors, v.hold_seconds, v.required_installations); }

template<class A> void fields(A& a, Field<A, CommandObjectives>& v)
{ a(v.faction, v.sectors); }

template<class A> void fields(A& a, Field<A, WarResult>& v)
{ a(v.status, v.tick, v.cause); }

// Scoped ID enums admit every value of their underlying type.
template<class T> constexpr auto last_value(T) { return std::numeric_limits<std::underlying_type_t<T>>::max(); }
constexpr auto last_value(FactionId) { return FactionId::east; }
constexpr auto last_value(Terrain) { return Terrain::water; }
constexpr auto last_value(ControlState) { return ControlState::unstable; }
constexpr auto last_value(Resource) { return Resource::ammunition; }
constexpr auto last_value(ShipmentStatus) { return ShipmentStatus::lost; }
constexpr auto last_value(LogisticsDelay) { return LogisticsDelay::closed; }
constexpr auto last_value(LogisticsEventKind) { return LogisticsEventKind::crossed; }
constexpr auto last_value(EquipmentCategory) { return EquipmentCategory::air_defense_team; }
constexpr auto last_value(EquipmentStatus) { return EquipmentStatus::lost; }
constexpr auto last_value(ServiceKind) { return ServiceKind::repair; }
constexpr auto last_value(ForcesEventKind) { return ForcesEventKind::arrival_delayed; }
constexpr auto last_value(MissionKind) { return MissionKind::attack; }
constexpr auto last_value(MissionStatus) { return MissionStatus::cancelled; }
constexpr auto last_value(GroundStatus) { return GroundStatus::destroyed; }
constexpr auto last_value(WarStatus) { return WarStatus::draw; }
constexpr auto last_value(geography::SiteKind) { return geography::SiteKind::urban_land; }
constexpr auto last_value(geography::OsmElement) { return geography::OsmElement::relation; }
constexpr auto last_value(geography::Cover) { return geography::Cover::urban; }
constexpr auto last_value(geography::Surface) { return geography::Surface::gravel; }
constexpr auto last_value(geography::Direction) { return geography::Direction::reverse; }
constexpr auto last_value(geography::Crossing) { return geography::Crossing::bridge; }
constexpr auto last_value(geography::RouteStatus) { return geography::RouteStatus::limit_exceeded; }

class Writer {
public:
    static constexpr bool reading = false;
    static constexpr auto version = schema_version;
    std::string bytes;

    void operator()(const auto&... values) { (write(values), ...); }

private:
    void write(bool value) { bytes.push_back(value ? 1 : 0); }

    // All integers use eight little-endian bytes, including counters of type size_t.
    void number(std::uint64_t value)
    {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            bytes.push_back(static_cast<char>(static_cast<unsigned char>(value >> shift)));
        }
    }

    template<class T> void write(const T& value)
    {
        if constexpr (std::is_enum_v<T>) {
            number(static_cast<std::uint64_t>(value));
        } else if constexpr (std::is_integral_v<T>) {
            number(static_cast<std::uint64_t>(value));
        } else {
            fields(*this, value);
        }
    }

    void write(const std::string& value) { number(value.size()); bytes.append(value); }

    template<class T> void write(const std::optional<T>& value)
    {
        write(value.has_value());
        if (value) {
            write(*value);
        }
    }

    template<class T, class U> void write(const std::pair<T, U>& value) { (*this)(value.first, value.second); }

    template<class T, std::size_t N> void write(const std::array<T, N>& values)
    {
        for (const auto& value : values) {
            write(value);
        }
    }

    template<class T> void write(const std::vector<T>& values)
    {
        number(values.size());
        for (const auto& value : values) {
            write(value);
        }
    }

    template<class K, class V> void write(const std::map<K, V>& values)
    {
        number(values.size());
        for (const auto& value : values) {
            write(value);
        }
    }
};

class Reader {
public:
    static constexpr bool reading = true;
    explicit Reader(std::string_view bytes, std::uint64_t schema = schema_version)
        : version(schema), remaining_(bytes) {}

    const std::uint64_t version;
    std::map<std::pair<FactionId, MissionId>, std::pair<std::size_t, std::uint64_t>> legacy_missions;
    std::map<UnitId, std::uint64_t> legacy_withdrawals;

    void operator()(auto&... values) { (read(values), ...); }

    std::string_view take(std::size_t count)
    {
        if (count > remaining_.size()) {
            invalid_checkpoint();
        }
        const auto result = remaining_.substr(0, count);
        remaining_.remove_prefix(count);
        return result;
    }

    bool done() const { return remaining_.empty(); }

private:
    std::string_view remaining_;

    std::uint64_t number()
    {
        const auto bytes = take(8);
        std::uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i) {
            value |= std::uint64_t{static_cast<unsigned char>(bytes[i])} << (8 * i);
        }
        return value;
    }

    std::size_t count()
    {
        const auto size = number();
        // Every stored element occupies at least one byte.
        if (size > remaining_.size()) {
            invalid_checkpoint();
        }
        return static_cast<std::size_t>(size);
    }

    void read(bool& value)
    {
        const auto byte = take(1).front();
        if (byte != 0 && byte != 1) {
            invalid_checkpoint();
        }
        value = byte != 0;
    }

    template<class T> void read(T& value)
    {
        if constexpr (std::is_enum_v<T>) {
            const auto stored = number();
            if (stored > static_cast<std::uint64_t>(last_value(T{}))) {
                invalid_checkpoint();
            }
            value = static_cast<T>(stored);
        } else if constexpr (std::is_integral_v<T>) {
            const auto word = number();
            if constexpr (std::is_signed_v<T>) {
                const auto stored = std::bit_cast<std::int64_t>(word);
                if (!std::in_range<T>(stored)) {
                    invalid_checkpoint();
                }
                value = static_cast<T>(stored);
            } else {
                if (!std::in_range<T>(word)) {
                    invalid_checkpoint();
                }
                value = static_cast<T>(word);
            }
        } else {
            fields(*this, value);
        }
    }

    void read(std::string& value) { value = take(count()); }

    template<class T> void read(std::optional<T>& value)
    {
        bool present{};
        read(present);
        if (present) {
            read(value.emplace());
        } else {
            value.reset();
        }
    }

    template<class T, class U> void read(std::pair<T, U>& value) { (*this)(value.first, value.second); }

    template<class T, std::size_t N> void read(std::array<T, N>& values)
    {
        for (auto& value : values) {
            read(value);
        }
    }

    template<class T> void read(std::vector<T>& values)
    {
        const auto size = count();
        for (std::size_t i = 0; i < size; ++i) {
            read(values.emplace_back());
        }
    }

    template<class K, class V> void read(std::map<K, V>& values)
    {
        const auto size = count();
        for (std::size_t i = 0; i < size; ++i) {
            K key{};
            read(key);
            if (!values.empty() && !(values.rbegin()->first < key)) {
                invalid_checkpoint();
            }
            read(values.try_emplace(key).first->second);
        }
    }
};

void write_world(Writer& archive, const WorldDefinition& world)
{
    archive(world.width(), world.height(), world.sector_size_m(), world.sectors().size());
    for (const auto& sector : world.sectors()) {
        archive(sector.properties);
    }
}

WorldDefinition read_world(Reader& archive)
{
    std::uint32_t width{}, height{}, size{};
    std::vector<SectorProperties> properties;
    archive(width, height, size, properties);
    return WorldDefinition(width, height, size, std::move(properties));
}

class StagedSave {
public:
    explicit StagedSave(const std::filesystem::path& destination)
    {
        for (std::uint64_t suffix = 0;; ++suffix) {
            directory_ = destination;
            directory_ += ".tmp." + std::to_string(suffix);
            if (std::filesystem::create_directory(directory_)) {
                break;
            }
        }
    }

    ~StagedSave()
    {
        std::error_code ignored;
        std::filesystem::remove(directory_ / "checkpoint", ignored);
        std::filesystem::remove(directory_, ignored);
    }

    std::filesystem::path file() const { return directory_ / "checkpoint"; }

private:
    std::filesystem::path directory_;
};

}  // namespace

struct CampaignArchive {
    template<class A>
    static void transfer(A& a, Field<A, CampaignState>& v)
    {
        a(v.seed_, v.generator_version_, v.scenario_fingerprint_, v.sector_states_,
          v.random_words_, v.random_index_, v.next_event_sequence_,
          v.economy_interval_ticks_, v.minimum_integrity_, v.stockpiles_, v.installations_, v.economy_ledger_,
          v.routes_, v.carriers_, v.shipments_, v.logistics_log_, v.resupply_, v.loading_ticks_, v.unloading_ticks_,
          v.equipment_types_, v.facilities_, v.units_, v.equipment_, v.forces_log_, v.operations_, v.strategies_,
          v.objectives_, v.missions_, v.operations_log_, v.combat_, v.combat_units_, v.battle_log_,
          v.victory_objectives_, v.command_objectives_, v.victory_since_, v.initial_strength_,
          v.capitulation_since_, v.surrenders_, v.war_result_);
        if (a.version != 1) {
            a(v.authored_geography_, v.ground_destinations_);
            if (v.authored_geography_) {
                a(v.movement_map_);
            }
        }

        // The two intelligence stores have private true-ID keys.
        if constexpr (A::reading) {
            for (auto faction : {FactionId::west, FactionId::east}) {
                bool present{};
                a(present);
                if (present) {
                    auto& store = v.intelligence_[faction];
                    a(store.sectors, store.contacts, store.installations);
                }
            }
        } else {
            for (auto faction : {FactionId::west, FactionId::east}) {
                const auto store = v.intelligence_.find(faction);
                a(store != v.intelligence_.end());
                if (store != v.intelligence_.end()) {
                    a(store->second.sectors, store->second.contacts, store->second.installations);
                }
            }
        }
    }

    static void restore_legacy_movement(CampaignState& v, const Reader& archive)
    {
        for (auto& [id, unit] : v.combat_units_) {
            unit.position = v.ground_anchor(v.units_.at(id).sector);
        }
        const auto restore = [&](CombatUnit& unit, std::span<const SectorId> steps, std::uint64_t progress) {
            for (std::size_t i = 1; i < steps.size(); ++i) {
                const auto& neighbors = v.world_.sector(steps[i - 1]).neighbors;
                if (std::ranges::find(neighbors, steps[i]) == neighbors.end()) {
                    invalid_checkpoint();
                }
            }
            const auto speed = std::max(1u, v.ground_speed(unit.unit));
            auto route = v.legacy_ground_route(*unit.position, steps, v.ground_anchor(steps.back()), speed);
            auto restored = geography::begin(route, 0);
            if (progress) {
                if (steps.size() < 2 || route.legs.empty()) {
                    invalid_checkpoint();
                }
                const auto& leg = route.legs.front();
                // M8 stored weighted distance in eighteenth-metres. The centreline
                // is known, so project the paid progress onto that first leg.
                const auto& cell = v.world_.sector(steps[1]);
                const auto weight = cell.properties.terrain == Terrain::forest ? 2 : 1;
                const auto old_distance = std::uint64_t{v.world_.sector_size_m()} * 18 * weight;
                if (progress >= old_distance) {
                    invalid_checkpoint();
                }
                restored.leg_elapsed_us = static_cast<std::int64_t>(
                    static_cast<__int128_t>(leg.duration_us) * progress / old_distance);
                restored.elapsed_us = restored.leg_elapsed_us;
                restored.entry_paid = true;
                restored.position = geography::position_at(leg, restored.leg_elapsed_us);
                v.move_combat_unit(unit, restored.position);
            }
            return GroundJourney{std::move(route), std::move(restored), speed};
        };
        for (const auto& [id, progress] : archive.legacy_withdrawals) {
            auto& unit = v.combat_units_.at(id);
            const std::array steps{v.units_.at(id).sector, *unit.withdrawal};
            unit.withdrawal_journey = restore(unit, steps, progress);
        }
        for (const auto& [key, movement] : archive.legacy_missions) {
            auto& mission = v.missions_.at(key);
            mission.ground_destination = v.ground_anchor(mission.target);
            if (mission.status != MissionStatus::scheduled && mission.status != MissionStatus::in_progress) {
                continue;
            }
            auto& unit = v.combat_units_.at(*mission.ground_unit);
            const auto [leg, progress] = movement;
            if (leg >= mission.ground_path.size() ||
                mission.ground_path.back() != mission.target ||
                mission.ground_path[leg] != v.units_.at(unit.unit).sector) {
                invalid_checkpoint();
            }
            mission.ground_journey = restore(unit, std::span{mission.ground_path}.subspan(leg), progress);
        }
    }

    static void restore_legacy_transport(CampaignState& v)
    {
        v.initialize_logistics_positions();
        for (auto& [id, shipment] : v.shipments_) {
            if (!shipment.edge) {
                continue;
            }
            if (!shipment.carrier) {
                invalid_checkpoint();
            }
            auto& carrier = v.carriers_.at(*shipment.carrier);
            const auto& edge = v.routes_.at(*shipment.edge);
            if (carrier.shipment != id || carrier.sector != edge.from || !shipment.remaining_ticks ||
                shipment.remaining_ticks > edge.travel_ticks) {
                invalid_checkpoint();
            }
            auto route = v.legacy_transport_leg(edge);
            auto progress = geography::begin(route, 0);
            const auto tick_us = std::int64_t{v.clock_.tick_seconds()} * 1'000'000;
            const auto elapsed = route.duration_us - __int128_t{shipment.remaining_ticks} * tick_us;
            if (elapsed < 0) {
                invalid_checkpoint();
            }
            progress.leg_elapsed_us = static_cast<std::int64_t>(elapsed);
            progress.elapsed_us = progress.leg_elapsed_us;
            progress.entry_paid = true;
            progress.position = geography::position_at(route.legs.front(), progress.leg_elapsed_us);
            carrier.position = progress.position;
            carrier.sector = static_cast<SectorId>(*v.movement_map_.sector(progress.position));
            const auto target = route.legs.back().to;
            shipment.journey = TransportJourney{std::move(route), std::move(progress), target};
        }
    }

    static void validate(const CampaignState& v)
    {
        if (v.sector_states_.size() != v.world_.sectors().size() || v.random_index_ >= v.random_words_.size() ||
            v.victory_since_.size() != v.victory_objectives_.size() || !v.economy_interval_ticks_ ||
            !v.loading_ticks_ || !v.unloading_ticks_ || (v.combat_.enabled && !v.combat_.interval_ticks) ||
            v.war_result_.tick > v.clock_.tick()) {
            invalid_checkpoint();
        }
        if (v.operations_.enabled &&
            (!v.operations_.territory_half_life_seconds || !v.operations_.contact_half_life_seconds ||
             !v.operations_.installation_half_life_seconds || v.intelligence_.size() != 2)) {
            invalid_checkpoint();
        }
        const auto sector = [&](SectorId id) { (void)v.world_.sector(id); };
        const auto& map = v.movement_map_;
        const auto geometry = v.movement_geometry();
        if (map.columns != geometry.columns || map.rows != geometry.rows ||
            map.sector_mm != geometry.sector_mm || map.tile_mm <= 0) {
            invalid_checkpoint();
        }
        for (const auto& line : map.waterways)
            if (!map.valid_position(line.from) || !map.valid_position(line.to) ||
                line.from.elevation || line.to.elevation || line.from == line.to)
                invalid_checkpoint();
        std::optional<std::pair<geography::OsmElement, std::uint64_t>> previous_feature;
        for (const auto& feature : map.site_features) {
            const auto key = std::pair{feature.element, feature.source_id};
            if (!feature.source_id || !map.valid_position(feature.position) ||
                (previous_feature && key <= *previous_feature))
                invalid_checkpoint();
            previous_feature = key;
        }
        for (const auto& [id, point] : v.ground_destinations_) {
            if (!map.valid_position(point) || map.sector(point) != static_cast<std::uint32_t>(id)) {
                invalid_checkpoint();
            }
        }
        const auto check_journey = [&](const geography::Route& route, const geography::Progress& progress,
                                       geography::Point destination) {
            if (route.status != geography::RouteStatus::found ||
                !map.valid_position(route.origin) || progress.leg > route.legs.size()) {
                invalid_checkpoint();
            }
            auto point = route.origin;
            __int128_t time = 0;
            __int128_t fuel = 0;
            __int128_t elapsed = 0;
            auto scheduled_position = point;
            for (std::size_t i = 0; i < route.legs.size(); ++i) {
                const auto& leg = route.legs[i];
                if (leg.from != point || !map.valid_position(leg.to) || leg.duration_us <= 0 ||
                    leg.fuel_ml < 0 || leg.distance_mm <= 0 ||
                    (leg.road && std::ranges::none_of(map.roads, [&](const auto& road) {
                        return road.id == *leg.road;
                    }))) {
                    invalid_checkpoint();
                }
                if (i == progress.leg) {
                    if (progress.leg_elapsed_us < 0 || progress.leg_elapsed_us >= leg.duration_us ||
                        (progress.leg_elapsed_us > 0 && !progress.entry_paid)) {
                        invalid_checkpoint();
                    }
                    elapsed = time + progress.leg_elapsed_us;
                    scheduled_position = geography::position_at(leg, progress.leg_elapsed_us);
                }
                point = leg.to;
                time += leg.duration_us;
                fuel += leg.fuel_ml;
            }
            if (progress.leg == route.legs.size()) {
                if (progress.leg_elapsed_us || progress.entry_paid) {
                    invalid_checkpoint();
                }
                scheduled_position = point;
                elapsed = time;
            }
            if (point != destination || scheduled_position != progress.position || time != route.duration_us ||
                fuel != route.fuel_ml || elapsed != progress.elapsed_us) {
                invalid_checkpoint();
            }
        };
        for (const auto& [id, stock] : v.stockpiles_) {
            sector(stock.sector);
            for (std::size_t i = 0; i < stock.quantity.size(); ++i) {
                if (stock.reserved[i] > stock.quantity[i] || stock.quantity[i] > stock.capacity[i]) {
                    invalid_checkpoint();
                }
            }
            const bool mobile = v.mobile_stocks_.contains(id);
            if (id != stock.id || mobile == stock.access_point.has_value() ||
                (stock.access_point && (!map.valid_position(*stock.access_point) ||
                 map.sector(*stock.access_point) != static_cast<std::uint32_t>(stock.sector)))) {
                invalid_checkpoint();
            }
        }
        for (const auto& site : v.installations_) {
            (void)v.stockpiles_.at(site.installation.stockpile);
        }
        for (const auto& [id, route] : v.routes_) {
            sector(route.from);
            sector(route.to);
            if (id != route.id || route.from == route.to || !route.travel_ticks || !route.slots) {
                invalid_checkpoint();
            }
        }
        for (const auto& [id, carrier] : v.carriers_) {
            sector(carrier.sector);
            if (id != carrier.id || carrier.fuel > carrier.tank_capacity || carrier.spent_ml >= 1000 ||
                (!carrier.fuel && carrier.spent_ml) || !carrier.position || !map.valid_position(*carrier.position) ||
                map.sector(*carrier.position) != static_cast<std::uint32_t>(carrier.sector) ||
                (carrier.shipment && v.shipments_.at(*carrier.shipment).carrier != id)) {
                invalid_checkpoint();
            }
        }
        std::map<RouteId, std::uint32_t> edge_commitments;
        std::map<std::uint32_t, std::uint32_t> road_commitments;
        for (const auto& [id, shipment] : v.shipments_) {
            (void)v.stockpiles_.at(shipment.request.origin);
            (void)v.stockpiles_.at(shipment.request.destination);
            if (id != shipment.request.id || (shipment.edge && !v.routes_.contains(*shipment.edge)) ||
                (shipment.carrier && !v.carriers_.contains(*shipment.carrier))) {
                invalid_checkpoint();
            }
            if (shipment.journey) {
                const auto& journey = *shipment.journey;
                if (!shipment.carrier || v.carriers_.at(*shipment.carrier).shipment != id ||
                    v.carriers_.at(*shipment.carrier).position != journey.progress.position ||
                    journey.progress.leg >= journey.route.legs.size() || !map.valid_position(journey.destination)) {
                    invalid_checkpoint();
                }
                check_journey(journey.route, journey.progress, journey.route.legs.back().to);
                if (shipment.edge && (shipment.road || !journey.progress.entry_paid ||
                    ++edge_commitments[*shipment.edge] > v.routes_.at(*shipment.edge).slots)) {
                    invalid_checkpoint();
                }
                if (shipment.road) {
                    const auto road = std::ranges::find(map.roads, *shipment.road, &geography::RoadSegment::id);
                    if (road == map.roads.end() || journey.route.legs[journey.progress.leg].road != shipment.road ||
                        ++road_commitments[*shipment.road] > road->slots) {
                        invalid_checkpoint();
                    }
                }
            } else if (shipment.edge || shipment.road) {
                invalid_checkpoint();
            }
        }
        for (const auto& policy : v.resupply_) {
            (void)v.stockpiles_.at(policy.origin);
            (void)v.stockpiles_.at(policy.destination);
        }
        for (const auto& [id, facility] : v.facilities_) {
            if (id != facility.id || !v.stockpiles_.contains(facility.stockpile)) {
                invalid_checkpoint();
            }
        }
        for (const auto& [id, unit] : v.units_) {
            sector(unit.sector);
            if (id != unit.id || (unit.facility && !v.facilities_.contains(*unit.facility))) {
                invalid_checkpoint();
            }
        }
        for (const auto& [id, item] : v.equipment_) {
            const auto& type = v.equipment_types_.at(item.assignment.type);
            if (id != item.assignment.id || !v.units_.contains(item.assignment.unit) ||
                item.last_service_seconds > item.operating_seconds || item.damage > 100 ||
                (item.job && item.job->work_seconds > service_seconds(type, item.job->kind))) {
                invalid_checkpoint();
            }
        }
        for (const auto& [id, unit] : v.combat_units_) {
            if (id != unit.unit || !v.units_.contains(id) || !v.stockpiles_.contains(unit.stores) ||
                !unit.position || !map.valid_position(*unit.position) || map.sector(*unit.position) !=
                    static_cast<std::uint32_t>(v.units_.at(id).sector) ||
                v.stockpiles_.at(unit.stores).sector != v.units_.at(id).sector ||
                unit.movement_fuel_credit_ml >= 1000 ||
                unit.withdrawal.has_value() != unit.withdrawal_journey.has_value()) {
                invalid_checkpoint();
            }
            if (unit.withdrawal) {
                sector(*unit.withdrawal);
                const auto& journey = *unit.withdrawal_journey;
                if (!journey.planned_speed_kph) {
                    invalid_checkpoint();
                }
                check_journey(journey.route, journey.progress, v.ground_anchor(*unit.withdrawal));
                if (unit.position != unit.withdrawal_journey->progress.position) {
                    invalid_checkpoint();
                }
            }
        }
        for (const auto& [key, mission] : v.missions_) {
            sector(mission.target);
            if (key != std::pair{mission.faction, mission.id} ||
                (mission.ground_unit && !v.combat_units_.contains(*mission.ground_unit)) ||
                !v.equipment_.contains(mission.equipment) ||
                !v.stockpiles_.contains(mission.launch_stock)) {
                invalid_checkpoint();
            }
            if (mission.ground_unit) {
                const bool active = mission.status == MissionStatus::scheduled ||
                    mission.status == MissionStatus::in_progress;
                if (!mission.ground_destination || !map.valid_position(*mission.ground_destination) ||
                    map.sector(*mission.ground_destination) != static_cast<std::uint32_t>(mission.target) ||
                    (active && !mission.ground_journey)) {
                    invalid_checkpoint();
                }
                if (mission.ground_journey) {
                    const auto& journey = *mission.ground_journey;
                    if (!journey.planned_speed_kph) {
                        invalid_checkpoint();
                    }
                    check_journey(journey.route, journey.progress, *mission.ground_destination);
                    if (active && v.combat_units_.at(*mission.ground_unit).position !=
                            mission.ground_journey->progress.position) {
                        invalid_checkpoint();
                    }
                }
            }
            for (auto step : mission.ground_path) {
                sector(step);
            }
        }
        const auto log_size = v.economy_ledger_.size() + v.logistics_log_.size() + v.forces_log_.size() +
                              v.operations_log_.size() + v.battle_log_.size();
        if (log_size != v.next_event_sequence_) {
            invalid_checkpoint();
        }
        std::vector<bool> seen(log_size);
        const auto check_log = [&](const auto& log) {
            std::optional<std::uint64_t> previous;
            for (const auto& event : log) {
                if (event.tick > v.clock_.tick() || event.sequence >= seen.size() ||
                    seen[event.sequence] || (previous && *previous >= event.sequence)) {
                    invalid_checkpoint();
                }
                seen[event.sequence] = true;
                previous = event.sequence;
            }
        };
        check_log(v.economy_ledger_);
        check_log(v.logistics_log_);
        check_log(v.forces_log_);
        check_log(v.operations_log_);
        check_log(v.battle_log_);
        for (const auto& event : v.battle_log_) {
            if (event.movement_offset_us &&
                (!event.unit || *event.movement_offset_us < 0 ||
                 *event.movement_offset_us > std::int64_t{v.clock_.tick_seconds()} * 1'000'000)) {
                invalid_checkpoint();
            }
        }
    }
};

std::uint64_t CampaignState::scenario_fingerprint(const ScenarioDefinition& s)
{
    Writer a;
    write_world(a, s.world);
    a(s.initial_sectors, s.tick_seconds, s.economy_interval_ticks, s.minimum_integrity, s.stockpiles,
      s.installations, s.routes, s.carriers, s.loading_ticks, s.unloading_ticks, s.equipment_types,
      s.facilities, s.units, s.equipment, s.operations, s.objectives, s.combat, s.combat_units,
      s.public_sectors, s.victory_objectives, s.command_objectives, s.resupply, s.initial_supply_requests,
      s.geographic_world, s.ground_destinations);
    return fingerprint(a.bytes);
}

void CampaignState::save(const std::filesystem::path& path) const
{
    Writer archive;
    archive.bytes = signature;
    archive(schema_version, rules_version, clock_.tick_seconds(), clock_.tick());
    write_world(archive, world_);
    CampaignArchive::transfer(archive, *this);
    archive(fingerprint(archive.bytes));
    if (!std::in_range<std::streamsize>(archive.bytes.size())) {
        throw std::runtime_error("Campaign checkpoint is too large to write");
    }

    const StagedSave staged(path);
    std::ofstream output(staged.file(), std::ios::binary);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.write(archive.bytes.data(), static_cast<std::streamsize>(archive.bytes.size()));
    output.close();
    std::filesystem::rename(staged.file(), path);
}

CampaignState CampaignState::load(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto length = static_cast<std::streamoff>(input.tellg());
    if (length < 0) {
        throw std::runtime_error("Cannot open campaign checkpoint: " + path.string());
    }
    if (!std::in_range<std::size_t>(length) || !std::in_range<std::streamsize>(length)) {
        throw std::runtime_error("Campaign checkpoint is too large to read");
    }
    input.seekg(0);
    std::array<char, header_size> header{};
    if (static_cast<std::size_t>(length) < header_size + checksum_size ||
        !input.read(header.data(), header.size())) {
        invalid_checkpoint();
    }
    Reader archive({header.data(), header.size()});
    if (archive.take(signature.size()) != signature) {
        invalid_checkpoint();
    }
    std::uint64_t schema{}, rules{};
    archive(schema, rules);
    if ((schema != schema_version || rules != rules_version) &&
        (schema != 3 || (rules != 3 && rules != 4)) &&
        (schema != 2 || rules != 2) && (schema != 1 || rules != 1)) {
        throw std::runtime_error("Unsupported campaign checkpoint schema or rules version");
    }
    std::string bytes(header.begin(), header.end());
    bytes.resize(static_cast<std::size_t>(length));
    if (!input.read(bytes.data() + header_size, static_cast<std::streamsize>(bytes.size() - header_size))) {
        invalid_checkpoint();
    }
    Reader checksum(std::string_view(bytes).substr(bytes.size() - checksum_size));
    std::uint64_t expected{};
    checksum(expected);
    const auto payload = std::string_view(bytes).substr(0, bytes.size() - checksum_size);
    if (expected != fingerprint(payload)) {
        throw std::runtime_error("Campaign checkpoint checksum mismatch");
    }

    Reader data(payload.substr(header_size), schema);
    std::uint32_t tick_seconds{};
    std::uint64_t tick{};
    data(tick_seconds, tick);
    SimulationClock clock(tick_seconds);
    clock.advance(tick);
    CampaignState campaign(read_world(data), clock);
    CampaignArchive::transfer(data, campaign);
    if (!data.done()) {
        invalid_checkpoint();
    }
    if (!campaign.authored_geography_) {
        campaign.movement_map_ = campaign.movement_geometry();
    }
    if (schema == 1) {
        CampaignArchive::restore_legacy_movement(campaign, data);
    }
    if (schema < 3) {
        CampaignArchive::restore_legacy_transport(campaign);
    } else {
        for (const auto& [id, unit] : campaign.combat_units_) {
            campaign.mobile_stocks_.emplace(unit.stores, id);
        }
    }
    CampaignArchive::validate(campaign);
    campaign.index_routes();
    return campaign;
}

}  // namespace openwar
