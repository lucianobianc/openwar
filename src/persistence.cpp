#include "campaign.hpp"

#include <bit>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace openwar {
namespace {

constexpr std::string_view signature = "OPENWAR-CHECKPOINT\n";
// Change the schema version for layout changes and the rules version for changes to simulation outcomes.
constexpr std::uint64_t schema_version = 1;
constexpr std::uint64_t rules_version = 1;
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
{ a(v.id, v.sector, v.owner, v.capacity, v.quantity, v.reserved); }

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
{ a(v.id, v.faction, v.sector, v.capacity, v.fuel, v.shipment, v.lost, v.tank_capacity); }

template<class A> void fields(A& a, Field<A, SupplyRequest>& v)
{ a(v.id, v.origin, v.destination, v.resource, v.quantity, v.priority, v.deadline); }

template<class A> void fields(A& a, Field<A, ResupplyPolicy>& v)
{ a(v.origin, v.destination, v.resource, v.threshold, v.target, v.priority); }

template<class A> void fields(A& a, Field<A, Shipment>& v)
{ a(v.request, v.status, v.carrier, v.edge, v.remaining_ticks, v.cargo, v.pickup_fuel); }

template<class A> void fields(A& a, Field<A, LogisticsEvent>& v)
{ a(v.tick, v.shipment, v.kind, v.quantity, v.sector, v.sequence); }

template<class A> void fields(A& a, Field<A, ServiceProfile>& v)
{ a(v.seconds, v.costs, v.technicians); }

template<class A> void fields(A& a, Field<A, EquipmentType>& v)
{
    a(v.id, v.name, v.faction, v.category, v.preparation, v.inspection, v.scheduled, v.repair,
      v.service_interval_seconds, v.operation_range, v.combat_power, v.can_occupy, v.ground_speed_kph);
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
      v.launch_tick, v.detections, v.manual, v.relocation, v.ground_unit, v.ground_path, v.ground_leg,
      v.preparation_reserved, v.ground_progress, v.earliest_launch_tick);
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
      v.suppression, v.withdrawal_progress);
}

template<class A> void fields(A& a, Field<A, BattleEvent>& v)
{ a(v.tick, v.sector, v.reason, v.unit, v.amount, v.sequence, v.previous_controller); }

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
constexpr auto last_value(LogisticsEventKind) { return LogisticsEventKind::refuelled; }
constexpr auto last_value(EquipmentCategory) { return EquipmentCategory::air_defense_team; }
constexpr auto last_value(EquipmentStatus) { return EquipmentStatus::lost; }
constexpr auto last_value(ServiceKind) { return ServiceKind::repair; }
constexpr auto last_value(ForcesEventKind) { return ForcesEventKind::arrival_delayed; }
constexpr auto last_value(MissionKind) { return MissionKind::attack; }
constexpr auto last_value(MissionStatus) { return MissionStatus::cancelled; }
constexpr auto last_value(GroundStatus) { return GroundStatus::destroyed; }
constexpr auto last_value(WarStatus) { return WarStatus::draw; }

class Writer {
public:
    static constexpr bool reading = false;
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
    explicit Reader(std::string_view bytes) : remaining_(bytes) {}

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
        for (const auto& [id, stock] : v.stockpiles_) {
            sector(stock.sector);
            for (std::size_t i = 0; i < stock.quantity.size(); ++i) {
                if (stock.reserved[i] > stock.quantity[i] || stock.quantity[i] > stock.capacity[i]) {
                    invalid_checkpoint();
                }
            }
            if (id != stock.id) {
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
            if (id != carrier.id || carrier.fuel > carrier.tank_capacity ||
                (carrier.shipment && v.shipments_.at(*carrier.shipment).carrier != id)) {
                invalid_checkpoint();
            }
        }
        for (const auto& [id, shipment] : v.shipments_) {
            (void)v.stockpiles_.at(shipment.request.origin);
            (void)v.stockpiles_.at(shipment.request.destination);
            if (id != shipment.request.id || (shipment.edge && !v.routes_.contains(*shipment.edge)) ||
                (shipment.carrier && !v.carriers_.contains(*shipment.carrier))) {
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
            if (id != unit.unit || !v.units_.contains(id) || !v.stockpiles_.contains(unit.stores)) {
                invalid_checkpoint();
            }
            if (unit.withdrawal) {
                sector(*unit.withdrawal);
            }
        }
        for (const auto& [key, mission] : v.missions_) {
            sector(mission.target);
            if (key != std::pair{mission.faction, mission.id} ||
                (mission.ground_unit && !v.combat_units_.contains(*mission.ground_unit)) ||
                (!mission.ground_unit && !v.equipment_.contains(mission.equipment)) ||
                !v.stockpiles_.contains(mission.launch_stock) || mission.ground_leg > mission.ground_path.size()) {
                invalid_checkpoint();
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
    }
};

std::uint64_t CampaignState::scenario_fingerprint(const ScenarioDefinition& s)
{
    Writer a;
    write_world(a, s.world);
    a(s.initial_sectors, s.tick_seconds, s.economy_interval_ticks, s.minimum_integrity, s.stockpiles,
      s.installations, s.routes, s.carriers, s.loading_ticks, s.unloading_ticks, s.equipment_types,
      s.facilities, s.units, s.equipment, s.operations, s.objectives, s.combat, s.combat_units,
      s.public_sectors, s.victory_objectives, s.command_objectives, s.resupply, s.initial_supply_requests);
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
    if (schema != schema_version || rules != rules_version) {
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

    Reader data(payload.substr(header_size));
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
    CampaignArchive::validate(campaign);
    campaign.index_routes();
    return campaign;
}

}  // namespace openwar
