#ifndef OPENWAR_CAMPAIGN_HPP
#define OPENWAR_CAMPAIGN_HPP

#include "operations.hpp"
#include "combat.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

enum class ShipmentStatus { pending, loading, en_route, delayed, unloading, delivered, cancelled, lost };
std::string_view shipment_status_name(ShipmentStatus status);

struct Shipment {
    SupplyRequest request;
    ShipmentStatus status = ShipmentStatus::pending;
    std::optional<CarrierId> carrier{};
    std::optional<RouteId> edge{};
    std::uint32_t remaining_ticks = 0;
    std::uint64_t cargo = 0;
};

enum class LogisticsEventKind {
    requested, assigned, loaded, delayed, unloaded, delivered, cancelled, lost, arrived
};

struct LogisticsEvent {
    std::uint64_t tick;
    ShipmentId shipment;
    LogisticsEventKind kind;
    std::uint64_t quantity = 0;
    std::optional<SectorId> sector{};
    bool operator==(const LogisticsEvent&) const = default;
};

enum class UnitId : std::uint32_t {};
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
};

struct UnitReadiness {
    std::size_t active = 0;
    std::size_t ready = 0;
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
    bool operator==(const ForcesEvent&) const = default;
};

std::string_view equipment_status_name(EquipmentStatus status);
std::string_view service_name(ServiceKind kind);
std::uint32_t service_seconds(const EquipmentType& type, ServiceKind kind);

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
    CampaignState(ScenarioDefinition scenario, std::uint32_t seed);

    const WorldDefinition& world() const { return world_; }
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
    void set_route_passable(RouteId id, bool passable);
    bool cancel_shipment(ShipmentId id);
    bool lose_carrier(CarrierId id);
    const auto& shipments() const { return shipments_; }
    const auto& carriers() const { return carriers_; }
    std::span<const LogisticsEvent> logistics_log() const { return logistics_log_; }
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
    OrderResult issue_order(FactionId actor, MissionKind kind, SectorId target);
    bool cancel_order(FactionId actor, MissionId id);
    bool combat_enabled() const { return combat_.enabled; }
    bool war_ended() const;
    const WarResult& war_result() const { return war_result_; }
    bool declare_surrender(FactionId actor);
    const auto& combat_units() const { return combat_units_; }
    std::span<const BattleEvent> observer_battle_log() const { return battle_log_; }
    std::vector<std::pair<SectorId, SectorId>> front_line() const;

private:
    void initialize_economy(ScenarioDefinition& scenario);
    bool update_economy(std::uint64_t tick);
    bool consume_at(StockpileId id, const ResourceAmounts& amounts, std::uint64_t tick,
                    std::optional<EquipmentId> equipment = {});
    void initialize_logistics(const ScenarioDefinition& scenario);
    bool update_logistics(std::uint64_t tick);
    bool advance_shipment(Shipment& shipment, std::uint64_t tick,
                          std::map<RouteId, std::uint32_t>& available_slots);
    bool route_usable(const TransportRoute& route, FactionId faction) const;
    std::optional<RouteId> next_route(SectorId from, SectorId to, FactionId faction) const;
    void initialize_forces(const ScenarioDefinition& scenario);
    bool update_forces(std::uint64_t tick);
    void set_equipment_status(Equipment& equipment, EquipmentStatus status, std::uint64_t tick);
    void queue_service(Equipment& equipment, ServiceKind kind, std::uint64_t tick);
    void initialize_operations(const ScenarioDefinition& scenario);
    bool update_operations();
    OrderResult schedule_mission(FactionId faction, MissionKind kind, SectorId target, bool manual);
    std::uint32_t observe(FactionId faction, SectorId sector);
    MissionId next_mission_id(FactionId faction) const;
    void initialize_combat(const ScenarioDefinition& scenario);
    OrderResult schedule_ground_mission(FactionId faction, MissionKind kind, SectorId target, bool manual);
    void update_combat();
    void move_combat_unit(CombatUnit& unit, SectorId sector);
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
    };

    WorldDefinition world_;
    SimulationClock clock_;
    std::vector<SectorState> sector_states_;
    std::mt19937 random_;
    std::uint32_t economy_interval_ticks_;
    std::uint32_t minimum_integrity_;
    std::map<StockpileId, Stockpile> stockpiles_;
    std::vector<InstallationState> installations_;
    std::vector<EconomyEntry> economy_ledger_;
    std::map<RouteId, TransportRoute> routes_;
    std::map<CarrierId, Carrier> carriers_;
    std::map<ShipmentId, Shipment> shipments_;
    std::vector<LogisticsEvent> logistics_log_;
    std::uint32_t loading_ticks_;
    std::uint32_t unloading_ticks_;
    std::map<EquipmentTypeId, EquipmentType> equipment_types_;
    std::map<FacilityId, MaintenanceFacility> facilities_;
    std::map<UnitId, MilitaryUnit> units_;
    std::map<EquipmentId, Equipment> equipment_;
    std::vector<ForcesEvent> forces_log_;
    OperationsSettings operations_;
    std::vector<Objective> objectives_;
    std::map<FactionId, IntelStore> intelligence_;
    std::map<std::pair<FactionId, MissionId>, Mission> missions_;
    std::vector<OperationsEvent> operations_log_;
    CombatSettings combat_;
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

}  // namespace openwar

#endif
