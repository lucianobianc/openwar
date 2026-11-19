#ifndef OPENWAR_CAMPAIGN_HPP
#define OPENWAR_CAMPAIGN_HPP

#include "operations.hpp"
#include "combat.hpp"
#include "military_identity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace openwar {

enum class FactionId : std::uint8_t { neutral = 0, west = 1, east = 2 };
enum class SectorId : std::uint32_t {};
enum class Terrain { plains, forest, water };

std::string_view faction_name(FactionId faction);
std::string_view terrain_name(Terrain terrain);

struct SectorProperties {
    Terrain terrain = Terrain::plains;
    bool traversable = true;
};

struct Sector {
    SectorId id;
    std::uint32_t x;
    std::uint32_t y;
    SectorProperties properties;
    std::vector<SectorId> neighbors;
};

class WorldDefinition {
public:
    // Sector properties are supplied in row-major order.
    WorldDefinition(std::uint32_t width, std::uint32_t height,
                    std::uint32_t sector_size_m, std::vector<SectorProperties> properties);

    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }
    std::uint32_t sector_size_m() const { return sector_size_m_; }
    std::span<const Sector> sectors() const { return sectors_; }
    const Sector& sector(SectorId id) const;
    SectorId sector_id(std::uint32_t x, std::uint32_t y) const;

private:
    std::uint32_t width_;
    std::uint32_t height_;
    std::uint32_t sector_size_m_;
    std::vector<Sector> sectors_;
};

struct SectorState {
    FactionId controller = FactionId::neutral;
    std::uint32_t objective_points = 0;
    ControlState control = ControlState::neutral;
    std::optional<FactionId> occupier{};
    std::uint64_t occupation_since = 0;
    std::uint64_t last_change_tick = 0;
};

struct InitialSectorState {
    SectorId sector;
    SectorState state;
};

enum class Resource { oil_reserve, crude_oil, fuel, materials, spare_parts, supplies, ammunition, count };
using ResourceAmounts = std::array<std::uint64_t, static_cast<std::size_t>(Resource::count)>;
enum class StockpileId : std::uint32_t {};
enum class InstallationId : std::uint32_t {};
enum class RouteId : std::uint32_t {};
enum class CarrierId : std::uint32_t {};
enum class ShipmentId : std::uint32_t {};
enum class EquipmentId : std::uint32_t {};

std::string_view resource_name(Resource resource);

struct Stockpile {
    StockpileId id;
    SectorId sector;
    FactionId owner;
    ResourceAmounts capacity{};
    ResourceAmounts quantity{};
    ResourceAmounts reserved{};
    std::optional<geography::Point> access_point{};  // Fixed stores only.
};

struct ProductionRecipe {
    ResourceAmounts inputs{};
    ResourceAmounts outputs{};
    std::uint32_t intervals_per_batch = 1;
    std::uint32_t workers_required = 1;
    bool requires_power = true;
};

struct Installation {
    InstallationId id;
    StockpileId stockpile;
    std::string name;
    FactionId operator_faction;
    ProductionRecipe recipe;
    std::uint32_t workers = 1;
    bool has_power = true;
    std::uint32_t integrity = 100;
};

struct InstallationState {
    Installation installation;
    // Each interval contributes up to 100 work points, scaled by integrity.
    std::uint64_t work = 0;
    std::uint64_t reactivation_seconds = 0;
};

struct EconomyEntry {
    std::uint64_t tick;
    StockpileId stockpile;
    // Present for production. Direct consumption has no producing installation.
    std::optional<InstallationId> producer;
    ResourceAmounts consumed;
    ResourceAmounts produced;
    std::optional<ShipmentId> shipment{};
    std::optional<EquipmentId> equipment{};

    std::uint64_t sequence = 0;
    bool operator==(const EconomyEntry&) const = default;
};

struct TransportRoute {
    RouteId id;
    SectorId from;
    SectorId to;
    std::uint32_t travel_ticks;
    std::uint32_t slots = 1;
    std::uint32_t fuel_cost = 1;
    bool passable = true;
};

struct Carrier {
    CarrierId id;
    FactionId faction;
    SectorId sector;
    std::uint64_t capacity;
    std::uint64_t fuel;
    std::optional<ShipmentId> shipment{};
    bool lost = false;
    // Zero uses the starting fuel as the tank capacity.
    std::uint64_t tank_capacity = 0;
    std::optional<geography::Point> position{};
    std::optional<geography::Profile> mobility{};
    // Fraction of a litre already spent from the rounded-up fuel balance.
    std::uint16_t spent_ml = 0;
};

struct SupplyRequest {
    ShipmentId id;
    StockpileId origin;
    StockpileId destination;
    Resource resource;
    std::uint64_t quantity;
    int priority = 0;
    std::optional<std::uint64_t> deadline{};
};

struct ResupplyPolicy {
    StockpileId origin;
    StockpileId destination;
    Resource resource;
    std::uint64_t threshold;
    std::uint64_t target;
    int priority = 0;
};

enum class ShipmentStatus { pending, loading, en_route, delayed, unloading, delivered, cancelled, lost };
std::string_view shipment_status_name(ShipmentStatus status);

enum class LogisticsDelay {
    none, unreachable, search_limit, fuel, unsafe, capacity, recipient_unavailable, destination_full, closed
};

struct TransportJourney {
    geography::Route route;
    geography::Progress progress;
    geography::Point destination;
};

struct Shipment {
    SupplyRequest request;
    ShipmentStatus status = ShipmentStatus::pending;
    std::optional<CarrierId> carrier{};
    std::optional<RouteId> edge{};
    std::uint32_t remaining_ticks = 0;
    std::uint64_t cargo = 0;
    std::uint64_t pickup_fuel = 0;
    std::optional<TransportJourney> journey{};
    std::optional<std::uint32_t> road{};  // Authored segment capacity commitment.
    LogisticsDelay delay = LogisticsDelay::none;
    std::optional<FactionId> requester{};
};

enum class LogisticsEventKind {
    requested, assigned, loaded, delayed, unloaded, delivered, cancelled, lost, arrived, collecting, refuelled, crossed
};
std::string_view logistics_event_name(LogisticsEventKind kind);

struct LogisticsEvent {
    std::uint64_t tick;
    ShipmentId shipment;
    LogisticsEventKind kind;
    std::uint64_t quantity = 0;
    std::optional<SectorId> sector{};
    std::uint64_t sequence = 0;
    bool operator==(const LogisticsEvent&) const = default;
};

enum class UnitId : std::uint32_t {};
using SupplyTarget = std::variant<StockpileId, UnitId>;

struct SupplyOrderPreview {
    std::string_view reason;
    std::optional<StockpileId> destination{};
    std::optional<StockpileId> source{};
    std::uint64_t room = 0;
    std::uint64_t source_available = 0;
    std::uint64_t idle_capacity = 0;
};

struct SupplyOrderResult {
    std::optional<ShipmentId> shipment;
    std::string_view reason;
};

struct SupplyOrderSummary {
    FactionId requester;
    SupplyRequest request;
    ShipmentStatus status;
    std::optional<CarrierId> carrier;
    std::uint64_t cargo;
};
enum class EquipmentTypeId : std::uint32_t {};
enum class FacilityId : std::uint32_t {};
enum class EquipmentCategory { helicopter, tank, transport, air_defense_vehicle, air_defense_team };
enum class ServiceKind { preparation, inspection, scheduled, repair };
enum class EquipmentStatus {
    available, reserved, preparing, ready, in_operation, returning, inspection, maintenance,
    repair, awaiting_parts, awaiting_crew, grounded, reserve, in_transit, lost
};

struct ServiceProfile {
    std::uint32_t seconds = 60;
    ResourceAmounts costs{};
    std::uint32_t technicians = 1;
};

struct EquipmentType {
    EquipmentTypeId id;
    std::string name;
    FactionId faction;
    EquipmentCategory category;
    ServiceProfile preparation;
    ServiceProfile inspection;
    ServiceProfile scheduled;
    ServiceProfile repair;
    std::uint32_t service_interval_seconds;
    std::uint32_t operation_range = 0;
    std::uint32_t combat_power = 0;
    bool can_occupy = false;
    std::uint32_t ground_speed_kph = 0;
    std::optional<geography::Profile> mobility{};
};

struct MaintenanceFacility {
    FacilityId id;
    StockpileId stockpile;
    std::uint32_t bays;
    std::uint32_t technicians;
    bool operational = true;
    std::vector<EquipmentTypeId> supported_types{};  // Empty means all compatible types.
};

struct MilitaryUnit {
    UnitId id;
    std::string name;
    FactionId faction;
    SectorId sector;
    std::optional<UnitId> parent{};
    std::optional<FacilityId> facility{};
    MilitaryIdentity identity{};
};

struct UnitReadiness {
    std::size_t active = 0;
    std::size_t ready = 0;
    std::size_t lost = 0;
};

struct EquipmentAssignment {
    EquipmentId id;
    EquipmentTypeId type;
    UnitId unit;
    std::string reserve_source{};
    std::uint64_t operating_seconds = 0;
};

struct ServiceJob {
    ServiceKind kind;
    std::uint64_t requested_tick;
    std::uint32_t work_seconds = 0;
    bool started = false;
};

struct Equipment {
    EquipmentAssignment assignment;
    EquipmentStatus status = EquipmentStatus::available;
    std::uint64_t operating_seconds = 0;
    std::uint64_t last_service_seconds = 0;
    std::uint32_t damage = 0;
    std::optional<ServiceJob> job{};
    std::optional<std::uint64_t> reinforcement_eta{};
    bool arrival_delayed = false;
    bool needs_turnaround = false;
};

enum class ForcesEventKind { status, service_started, service_completed, arrival_delayed };

struct ForcesEvent {
    std::uint64_t tick;
    EquipmentId equipment;
    EquipmentStatus status;
    std::optional<ServiceKind> service{};
    ForcesEventKind kind = ForcesEventKind::status;
    std::uint64_t sequence = 0;
    bool operator==(const ForcesEvent&) const = default;
};

std::string_view equipment_status_name(EquipmentStatus status);
std::string_view service_name(ServiceKind kind);
std::uint32_t service_seconds(const EquipmentType& type, ServiceKind kind);

enum class StartingPreset { original, reduced, increased };

struct CampaignOptions {
    // WEST and EAST respectively.
    std::array<StartingPreset, 2> strength{};
    std::array<StartingPreset, 2> stocks{};
    std::uint32_t seed = 0;
    std::optional<std::uint32_t> duration_minutes{};
    std::optional<std::uint32_t> autonomous_orders{};
    bool operator==(const CampaignOptions&) const = default;
};

struct CampaignSetupRecord {
    std::string scenario;
    std::string world;
    CampaignOptions options;
    bool operator==(const CampaignSetupRecord&) const = default;
};

struct ScenarioDefinition {
    WorldDefinition world;
    // Unlisted sectors start neutral.
    std::vector<InitialSectorState> initial_sectors;
    std::uint32_t tick_seconds = 60;
    std::uint32_t economy_interval_ticks = 5;
    std::uint32_t minimum_integrity = 25;
    std::vector<Stockpile> stockpiles{};
    std::vector<Installation> installations{};
    std::vector<TransportRoute> routes{};
    std::vector<Carrier> carriers{};
    std::uint32_t loading_ticks = 1;
    std::uint32_t unloading_ticks = 1;
    std::vector<EquipmentType> equipment_types{};
    std::vector<MaintenanceFacility> facilities{};
    std::vector<MilitaryUnit> units{};
    std::vector<EquipmentAssignment> equipment{};
    OperationsSettings operations{};
    std::vector<Objective> objectives{};
    CombatSettings combat{};
    std::vector<CombatUnit> combat_units{};
    std::vector<SectorId> public_sectors{};
    std::vector<VictoryObjective> victory_objectives{};
    std::vector<CommandObjectives> command_objectives{};
    std::vector<ResupplyPolicy> resupply{};
    std::vector<SupplyRequest> initial_supply_requests{};
    std::optional<geography::Map> geographic_world{};
    std::map<SectorId, geography::Point> ground_destinations{};
    std::optional<CampaignSetupRecord> setup{};
};

class SimulationClock {
public:
    explicit SimulationClock(std::uint32_t tick_seconds);
    void advance(std::uint64_t ticks);
    std::uint64_t tick() const { return tick_; }
    std::uint32_t tick_seconds() const { return tick_seconds_; }
    std::uint64_t elapsed_seconds() const { return tick_ * tick_seconds_; }

private:
    std::uint32_t tick_seconds_;
    std::uint64_t tick_ = 0;
};

class CampaignState {
public:
    CampaignState(ScenarioDefinition scenario, std::uint32_t seed,
                  std::optional<std::uint64_t> procedural_seed = {}, std::uint32_t generator_version = 0);

    // A checkpoint owns the resolved world and resumes at the saved tick.
    void save(const std::filesystem::path& path) const;
    static CampaignState load(const std::filesystem::path& path);
    const std::optional<CampaignSetupRecord>& setup() const { return setup_; }
    std::uint64_t seed() const { return seed_; }
    std::uint32_t generator_version() const { return generator_version_; }
    bool has_operational_view() const { return operations_.enabled; }
    bool strategic_command_enabled() const { return operations_.strategy_review_ticks != 0; }
    std::optional<FactionId> player_faction() const { return operations_.player; }

    const WorldDefinition& world() const { return world_; }
    const geography::Map* geographic_world() const { return authored_geography_ ? &movement_map_ : nullptr; }
    const SimulationClock& clock() const { return clock_; }
    const SectorState& sector_state(SectorId sector) const;
    void advance_ticks(std::uint64_t ticks);
    std::uint32_t next_random();
    const std::map<StockpileId, Stockpile>& stockpiles() const { return stockpiles_; }
    std::span<const InstallationState> installations() const { return installations_; }
    std::span<const EconomyEntry> economy_ledger() const { return economy_ledger_; }
    bool consume(StockpileId stockpile, const ResourceAmounts& amounts);
    void set_operating_conditions(InstallationId id, bool has_power, std::uint32_t workers,
                                  std::uint32_t integrity);
    void request_supply(SupplyRequest request);
    SupplyOrderPreview preview_supply(FactionId actor, SupplyTarget target, Resource resource,
                                      std::uint64_t quantity) const;
    SupplyOrderResult submit_supply(FactionId actor, SupplyTarget target, Resource resource, std::uint64_t quantity);
    bool can_cancel_supply(FactionId actor, ShipmentId id) const;
    bool cancel_supply(FactionId actor, ShipmentId id);
    std::vector<SupplyOrderSummary> supply_orders(std::optional<FactionId> perspective) const;
    void set_route_passable(RouteId id, bool passable);
    void set_road_closed(std::uint32_t id, bool closed);
    const auto& closed_roads() const { return closed_roads_; }
    geography::Point stockpile_position(StockpileId id) const;
    bool cancel_shipment(ShipmentId id);
    bool lose_carrier(CarrierId id);
    const auto& shipments() const { return shipments_; }
    const auto& carriers() const { return carriers_; }
    std::span<const LogisticsEvent> logistics_log() const { return logistics_log_; }
    const auto& facilities() const { return facilities_; }
    const auto& units() const { return units_; }
    const auto& equipment() const { return equipment_; }
    const auto& equipment_types() const { return equipment_types_; }
    UnitReadiness unit_readiness(UnitId id) const;
    std::span<const ForcesEvent> forces_log() const { return forces_log_; }
    bool reserve_equipment(EquipmentId id);
    bool cancel_equipment_reservation(EquipmentId id);
    bool prepare_equipment(EquipmentId id);
    bool begin_operation(EquipmentId id);
    bool finish_operation(EquipmentId id);
    bool damage_equipment(EquipmentId id, std::uint32_t damage);
    bool lose_equipment(EquipmentId id);
    bool authorize_reinforcement(EquipmentId id, std::uint64_t eta);
    void set_facility_conditions(FacilityId id, bool operational, std::uint32_t technicians);
    FactionView faction_view(FactionId faction, bool include_events = true) const;
    std::span<const OperationsEvent> observer_operations_log() const { return operations_log_; }
    bool can_issue_orders(FactionId actor) const;
    std::optional<bool> operational_reserve(FactionId actor, UnitId id) const;
    bool set_operational_reserve(FactionId actor, UnitId id, bool reserved);
    std::vector<MissionKind> available_orders(FactionId actor, UnitId unit) const;
    MissionPlan preview_order(FactionId actor, MissionKind kind, SectorId target,
                            std::optional<UnitId> formation = {}) const;
    MissionPlan preview_order(FactionId actor, MissionKind kind, geography::Point destination,
                            std::optional<UnitId> formation = {}, std::span<const geography::Point> via = {}) const;
    OrderResult issue_order(FactionId actor, MissionKind kind, SectorId target,
                            std::optional<UnitId> formation = {});
    OrderResult issue_ground_order(FactionId actor, MissionKind kind, geography::Point destination,
                            std::optional<UnitId> formation = {}, std::span<const geography::Point> via = {});
    bool can_cancel_order(FactionId actor, MissionId id) const;
    bool cancel_order(FactionId actor, MissionId id);
    bool combat_enabled() const { return combat_.enabled; }
    bool war_ended() const;
    const WarResult& war_result() const { return war_result_; }
    bool declare_surrender(FactionId actor);
    const auto& combat_units() const { return combat_units_; }
    std::optional<GroundMovement> ground_movement(UnitId id, bool faction_limited = false) const;
    std::span<const BattleEvent> observer_battle_log() const { return battle_log_; }
    std::vector<std::pair<SectorId, SectorId>> front_line() const;

private:
    friend struct CampaignArchive;
    CampaignState(WorldDefinition world, SimulationClock clock);
    static std::uint64_t scenario_fingerprint(const ScenarioDefinition& scenario);
    void index_routes();

    // Preserve emission order when a presentation merges subsystem journals.
    template<class Event>
    void record_event(std::vector<Event>& log, Event event)
    {
        event.sequence = next_event_sequence_;
        log.push_back(std::move(event));
        ++next_event_sequence_;
    }

    void initialize_economy(ScenarioDefinition& scenario);
    bool update_economy(std::uint64_t tick);
    bool consume_at(StockpileId id, const ResourceAmounts& amounts, std::uint64_t tick,
                    std::optional<EquipmentId> equipment = {}, bool from_reservation = false);
    void initialize_logistics(const ScenarioDefinition& scenario);
    void queue_supply(SupplyRequest request, std::uint64_t tick);
    void request_replenishment(std::uint64_t tick);
    void release_pickup_stock(Shipment& shipment);
    bool cancel_shipment_at(Shipment& shipment, std::uint64_t tick);
    bool update_logistics(std::uint64_t tick);
    bool advance_shipment(Shipment& shipment, std::uint64_t tick,
                          std::map<RouteId, std::uint32_t>& available_slots,
                          std::map<std::uint32_t, std::uint32_t>& road_slots);
    bool advance_transport(Shipment& shipment, Carrier& carrier, geography::Point target,
                           std::uint64_t tick, std::map<RouteId, std::uint32_t>& available_slots,
                           std::map<std::uint32_t, std::uint32_t>& road_slots);
    bool delay_shipment(Shipment& shipment, std::uint64_t tick, LogisticsDelay reason);
    bool route_usable(const TransportRoute& route, FactionId faction) const;
    bool transport_allowed(SectorId sector, FactionId faction) const;
    void initialize_logistics_positions();
    geography::Point logistics_anchor(SectorId sector) const;
    geography::Route legacy_transport_leg(const TransportRoute& route) const;
    geography::Route local_transport_leg(const Carrier& carrier, geography::Point from,
                                         geography::Point to) const;
    GroundStatus recipient_status(StockpileId stock) const;
    struct TransportPath {
        std::optional<RouteId> first{};
        std::uint64_t ticks = 0;
        std::uint64_t fuel = 0;
    };
    std::optional<TransportPath> transport_path(SectorId from, SectorId to, FactionId faction,
                                                bool cheapest = false, bool* limited = nullptr) const;
    struct TransportPlan {
        LogisticsDelay failure = LogisticsDelay::unreachable;
        std::int64_t duration_us{};
        std::int64_t fuel_ml{};
        std::optional<RouteId> edge{};
        geography::Route journey{};
    };
    TransportPlan transport_plan(const Carrier& carrier, geography::Point from, geography::Point to,
                                 std::int64_t budget, bool cheapest = false) const;
    TransportPlan delivery_plan(const Carrier& carrier, geography::Point from, geography::Point to,
                                geography::Point depot, std::int64_t budget) const;
    void initialize_forces(const ScenarioDefinition& scenario);
    bool update_forces(std::uint64_t tick);
    void set_equipment_status(Equipment& equipment, EquipmentStatus status, std::uint64_t tick);
    void queue_service(Equipment& equipment, ServiceKind kind, std::uint64_t tick);
    bool reinforcement_destination_active(UnitId id) const;
    bool reinforcement_entry_ready(const Equipment& item) const;
    void initialize_operations(const ScenarioDefinition& scenario);
    bool update_operations();
    bool update_strategy(FactionId faction);
    void dispatch_reinforcement(FactionId faction, const FactionView& view, const StrategicPlan& plan);
    OrderResult schedule_mission(FactionId faction, MissionKind kind, SectorId target, bool manual);
    bool formation_committed(UnitId unit) const;
    std::string_view order_authority_refusal(FactionId actor, std::optional<UnitId> formation) const;
    MissionPlan plan_sector_order(FactionId faction, MissionKind kind, SectorId target,
                                 std::optional<UnitId> formation = {}) const;
    MissionPlan plan_air_order(FactionId faction, MissionKind kind, SectorId target, const FactionView& view,
                              std::optional<UnitId> formation = {}) const;
    OrderResult commit_order(MissionPlan plan, bool manual);
    void release_unspent_mission_stock(Mission& mission);
    void release_air_mission_equipment(const Mission& mission, EquipmentStatus standby);
    enum class ObservationScope { sector_survey, territory, ground_recon };
    std::uint32_t observe(FactionId faction, SectorId sector, ObservationScope scope = ObservationScope::sector_survey);
    bool ground_recon_visibility(geography::Point point, SectorId sector) const;
    void update_ground_recon(Mission& mission);
    bool observe_road(FactionId faction, std::uint32_t road, SectorId sector, std::uint64_t tick);
    RoadReport aged_road_report(RoadReport report) const;
    bool road_known_closed(FactionId faction, std::uint32_t road) const;
    MissionId next_mission_id(FactionId faction) const;
    void initialize_combat(const ScenarioDefinition& scenario);
    MissionPlan plan_ground_order(FactionId faction, MissionKind kind, SectorId target,
                                  std::optional<geography::Point> destination = {},
                                  std::optional<UnitId> formation = {}, std::span<const geography::Point> via = {}) const;
    std::string_view ground_target_refusal(const FactionView& view, FactionId faction,
                                           MissionKind kind, SectorId target) const;
    std::optional<Mission> plan_ground_mission(FactionId faction, MissionKind kind, SectorId target,
                                              const FactionView& view, std::span<const UnitId> excluded,
                                              std::optional<geography::Point> destination = {},
                                              std::string_view* refusal = nullptr,
                                              std::optional<UnitId> formation = {},
                                              std::span<const geography::Point> via = {}) const;
    MissionId commit_ground_mission(Mission mission);
    void update_combat();
    void move_combat_unit(CombatUnit& unit, geography::Point position);
    geography::Point ground_anchor(SectorId sector) const;
    geography::Profile ground_profile(UnitId id) const;
    geography::Route ground_route(const CombatUnit& combat, geography::Point origin, geography::Point destination,
                                  const FactionView& view, std::vector<SectorId>& path, bool diagnose_fuel = false,
                                  std::optional<std::int64_t> fuel_budget = {}) const;
    geography::Route ground_itinerary(const CombatUnit& combat, geography::Point destination,
                                      std::span<const geography::Point> via, const FactionView& view,
                                      std::vector<SectorId>& path, std::vector<GroundWaypoint>& waypoints) const;
    void record_ground_waypoints(Mission& mission);
    geography::Map movement_geometry() const;
    geography::Route legacy_ground_route(geography::Point origin, std::span<const SectorId> steps,
                                         geography::Point destination, std::uint32_t speed) const;
    std::uint32_t ground_speed(UnitId id) const;
    std::optional<EquipmentId> ground_order_equipment(UnitId unit, MissionKind kind,
                                                       std::string_view* refusal = nullptr) const;
    bool ground_opposition(SectorId sector, FactionId faction) const;
    enum class GroundStep { no_fuel, blocked, territorial_access_lost, travelling, arrived };
    GroundStep advance_ground(CombatUnit& unit, GroundJourney& journey, bool withdrawing,
                              const geography::SectorAccess& sector_allowed = {},
                              std::optional<SectorId> observation_target = {}, bool patrol = false);
    void release_ground_equipment(EquipmentId id);
    void resolve_engagements();
    void update_territory();
    void capture_sector(SectorId sector, FactionId faction);
    void evaluate_war();
    void conclude_war(WarStatus status, std::string_view cause);
    struct IntelStore {
        std::map<SectorId, SectorReport> sectors;
        std::map<EquipmentId, ContactReport> contacts;  // True IDs remain internal to observation.
        std::map<SectorId, InstallationReport> installations;
        std::map<std::uint32_t, RoadReport> roads;
    };

    std::uint64_t seed_ = 0;
    std::uint32_t generator_version_ = 0;
    std::uint64_t scenario_fingerprint_ = 0;
    std::optional<CampaignSetupRecord> setup_;
    WorldDefinition world_;
    SimulationClock clock_;
    std::vector<SectorState> sector_states_;
    // MT19937 recurrence state, stored explicitly for portable checkpoints.
    std::array<std::uint32_t, 624> random_words_{};
    std::uint32_t random_index_ = 0;
    std::uint64_t next_event_sequence_ = 0;
    std::uint32_t economy_interval_ticks_ = 1;
    std::uint32_t minimum_integrity_ = 0;
    std::map<StockpileId, Stockpile> stockpiles_;
    std::vector<InstallationState> installations_;
    std::vector<EconomyEntry> economy_ledger_;
    std::map<RouteId, TransportRoute> routes_;
    std::map<SectorId, std::vector<RouteId>> incoming_routes_;
    std::map<RouteId, std::vector<SectorId>> transport_footprints_;
    std::map<CarrierId, Carrier> carriers_;
    std::map<StockpileId, UnitId> mobile_stocks_;  // Derived from combat unit assignments.
    std::map<ShipmentId, Shipment> shipments_;
    std::vector<LogisticsEvent> logistics_log_;
    std::vector<ResupplyPolicy> resupply_;
    std::uint32_t loading_ticks_ = 1;
    std::uint32_t unloading_ticks_ = 1;
    std::map<EquipmentTypeId, EquipmentType> equipment_types_;
    std::map<FacilityId, MaintenanceFacility> facilities_;
    std::map<UnitId, MilitaryUnit> units_;
    std::set<UnitId> operational_reserve_;
    std::map<EquipmentId, Equipment> equipment_;
    std::vector<ForcesEvent> forces_log_;
    OperationsSettings operations_;
    std::map<FactionId, StrategicPlan> strategies_;
    std::vector<Objective> objectives_;
    std::map<FactionId, IntelStore> intelligence_;
    std::map<std::pair<FactionId, MissionId>, Mission> missions_;
    std::vector<OperationsEvent> operations_log_;
    CombatSettings combat_;
    geography::Map movement_map_{};
    std::set<std::uint32_t> closed_roads_;
    bool authored_geography_ = false;
    std::map<SectorId, geography::Point> ground_destinations_;
    std::map<UnitId, CombatUnit> combat_units_;
    std::vector<BattleEvent> battle_log_;
    std::vector<VictoryObjective> victory_objectives_;
    std::vector<CommandObjectives> command_objectives_;
    std::vector<std::optional<std::uint64_t>> victory_since_;
    std::map<FactionId, std::uint64_t> initial_strength_;
    std::map<FactionId, std::uint64_t> capitulation_since_;
    std::vector<FactionId> surrenders_;
    WarResult war_result_{};
};

ScenarioDefinition example_scenario();
ScenarioDefinition logistics_example_scenario();
CampaignState maintenance_example_campaign(std::uint32_t seed);
CampaignState operations_example_campaign(std::uint32_t seed, std::optional<FactionId> player = {});
ScenarioDefinition combat_example_scenario(std::optional<FactionId> player = {});
CampaignState long_example_campaign(std::uint32_t seed, bool strategic_command = true);

}  // namespace openwar

#endif
