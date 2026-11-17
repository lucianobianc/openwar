#ifndef OPENWAR_CAMPAIGN_HPP
#define OPENWAR_CAMPAIGN_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
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
};

struct InitialSectorState {
    SectorId sector;
    SectorState state;
};

enum class Resource { oil_reserve, crude_oil, fuel, materials, spare_parts, supplies, count };
using ResourceAmounts = std::array<std::uint64_t, static_cast<std::size_t>(Resource::count)>;
enum class StockpileId : std::uint32_t {};
enum class InstallationId : std::uint32_t {};
enum class RouteId : std::uint32_t {};
enum class CarrierId : std::uint32_t {};
enum class ShipmentId : std::uint32_t {};

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
};

struct EconomyEntry {
    std::uint64_t tick;
    StockpileId stockpile;
    // Present for production. Direct consumption has no producing installation.
    std::optional<InstallationId> producer;
    ResourceAmounts consumed;
    ResourceAmounts produced;
    std::optional<ShipmentId> shipment{};

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

enum class LogisticsEventKind { requested, assigned, loaded, delayed, unloaded, delivered, cancelled, lost };

struct LogisticsEvent {
    std::uint64_t tick;
    ShipmentId shipment;
    LogisticsEventKind kind;
    std::uint64_t quantity = 0;
    bool operator==(const LogisticsEvent&) const = default;
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
};

class SimulationClock {
public:
    explicit SimulationClock(std::uint32_t tick_seconds);
    void advance(std::uint64_t ticks);
    std::uint64_t tick() const { return tick_; }
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

private:
    void initialize_economy(ScenarioDefinition& scenario);
    bool update_economy(std::uint64_t tick);
    void initialize_logistics(const ScenarioDefinition& scenario);
    bool update_logistics(std::uint64_t tick);
    bool advance_shipment(Shipment& shipment, std::uint64_t tick,
                          std::map<RouteId, std::uint32_t>& available_slots);
    bool route_usable(const TransportRoute& route, FactionId faction) const;
    std::optional<RouteId> next_route(SectorId from, SectorId to, FactionId faction) const;

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
};

ScenarioDefinition example_scenario();
ScenarioDefinition logistics_example_scenario();

}  // namespace openwar

#endif
