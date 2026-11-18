#include "realtime.hpp"

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace openwar::cli {
namespace {

volatile std::sig_atomic_t interrupted = 0;

void interrupt(int)
{
    interrupted = 1;
}

class InterruptHandler {
public:
    InterruptHandler() : previous_(std::signal(SIGINT, interrupt))
    {
        if (previous_ == SIG_ERR) {
            throw std::runtime_error("Could not install Ctrl+C handler");
        }
    }

    ~InterruptHandler() { std::signal(SIGINT, previous_); }

private:
    void (*previous_)(int);
};

bool supports_ansi()
{
#ifdef _WIN32
    return false;
#else
    const auto* term = std::getenv("TERM");
    return isatty(STDOUT_FILENO) && term && std::string_view(term) != "dumb";
#endif
}

class TerminalScreen {
public:
    explicit TerminalScreen(bool ansi) : ansi_(ansi)
    {
        if (ansi_) {
            std::cout << "\x1b[?1049h\x1b[?25l" << std::flush;
        }
    }

    ~TerminalScreen()
    {
        if (ansi_) {
            std::cout << "\x1b[?25h\x1b[?1049l" << std::flush;
        }
    }

    void draw(const std::string& frame) const
    {
        if (ansi_) {
            // Clear stale characters line by line without blanking the screen first.
            std::string output = "\x1b[H";
            std::size_t rows = 24;
            std::size_t columns = 80;
#ifndef _WIN32
            winsize size{};
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row && size.ws_col) {
                rows = size.ws_row;
                columns = size.ws_col;
            }
#endif
            std::istringstream lines(frame);
            std::string line;
            for (std::size_t row = 0; row < rows && std::getline(lines, line); ++row) {
                if (row != 0) {
                    output += "\r\n";
                }
                output += line.substr(0, columns - 1);
                output += "\x1b[K";
            }
            std::cout << output << "\x1b[J" << std::flush;
        } else {
            std::cout << frame << '\n' << std::flush;
        }
    }

private:
    bool ansi_;
};

std::string format_logistics_event(const LogisticsEvent& entry, std::uint32_t tick_seconds)
{
    std::ostringstream event;
    event << "Event tick " << entry.tick << " (" << entry.tick * tick_seconds
          << "s): shipment " << static_cast<std::uint32_t>(entry.shipment)
          << ' ' << logistics_event_name(entry.kind) << " [" << entry.quantity << ']';
    if (entry.sector) {
        event << " in sector " << static_cast<std::uint32_t>(*entry.sector);
    }
    return event.str();
}

struct EventCursor {
    std::size_t logistics = 0;
    std::size_t economy = 0;
    std::size_t forces = 0;

    template<class Output>
    void drain(const CampaignState& campaign, Output output)
    {
        const auto transport = campaign.logistics_log();
        const auto transactions = campaign.economy_ledger();
        const auto equipment = campaign.forces_log();
        while (logistics < transport.size() || economy < transactions.size() || forces < equipment.size()) {
            // Shipment debits and credits already have corresponding logistics events.
            if (economy < transactions.size() && transactions[economy].shipment) {
                ++economy;
                continue;
            }
            std::ostringstream event;
            if (forces < equipment.size() &&
                (logistics == transport.size() || equipment[forces].sequence < transport[logistics].sequence) &&
                (economy == transactions.size() || equipment[forces].sequence < transactions[economy].sequence)) {
                const auto& entry = equipment[forces++];
                event << "Event tick " << entry.tick << " (" << entry.tick * campaign.clock().tick_seconds()
                      << "s): equipment " << static_cast<std::uint32_t>(entry.equipment) << ' '
                      << equipment_status_name(entry.status);
                if (entry.service) {
                    event << " (" << service_name(*entry.service) << ')';
                }
                if (entry.kind == ForcesEventKind::service_started) {
                    event << " started";
                } else if (entry.kind == ForcesEventKind::service_completed) {
                    event << " completed";
                } else if (entry.kind == ForcesEventKind::arrival_delayed) {
                    event << " (entry unavailable)";
                }
            } else if (logistics < transport.size() &&
                (economy == transactions.size() || transport[logistics].sequence < transactions[economy].sequence)) {
                output(format_logistics_event(transport[logistics++], campaign.clock().tick_seconds()));
                continue;
            } else {
                const auto& entry = transactions[economy++];
                event << "Event tick " << entry.tick << " (" << entry.tick * campaign.clock().tick_seconds()
                      << "s): stockpile " << static_cast<std::uint32_t>(entry.stockpile);
                if (entry.producer) {
                    event << ", installation " << static_cast<std::uint32_t>(*entry.producer) << " completed batch";
                } else {
                    event << " consumed stock";
                    if (entry.equipment) {
                        event << " for equipment " << static_cast<std::uint32_t>(*entry.equipment);
                    }
                }
                for (std::size_t i = 0; i < entry.consumed.size(); ++i) {
                    if (entry.consumed[i] || entry.produced[i]) {
                        event << " | " << resource_name(static_cast<Resource>(i))
                              << " -" << entry.consumed[i] << "/+" << entry.produced[i];
                    }
                }
            }
            output(event.str());
        }
    }
};

void report_war(std::ostream& out, const CampaignState& campaign)
{
    const auto& result = campaign.war_result();
    out << "War: " << war_status_name(result.status);
    if (campaign.war_ended()) {
        out << " at tick " << result.tick << " (" << result.cause << ')';
    }
    out << '\n';
}

void report_movement(std::ostream& out, const GroundMovement& movement)
{
    out << " | next sector " << static_cast<std::uint32_t>(movement.next_sector)
        << " | " << movement.leg_percent << "% | ETA ";
    if (movement.eta_seconds) {
        out << *movement.eta_seconds << "s";
    } else {
        out << "unavailable";
    }
}

void report_strategy(std::ostream& out, FactionId faction, const StrategicPlan& strategy)
{
    out << faction_name(faction) << " strategy | " << (strategy.recovering ? "recovering" : "operational");
    if (strategy.focus) {
        out << " | focus sector " << static_cast<std::uint32_t>(*strategy.focus);
    }
    out << " | ground losses " << strategy.ground_losses << " | reviewed tick " << strategy.reviewed_tick << '\n';
}

void report_operations(std::ostream& out, const CampaignState& campaign, FactionId perspective, bool compact)
{
    if (perspective == FactionId::neutral) {
        out << "Operations observer | separate faction reports\n";
    }
    if (campaign.combat_enabled()) {
        report_war(out, campaign);
        if (perspective == FactionId::neutral) {
            out << "Front:";
            for (const auto& [left, right] : campaign.front_line()) {
                out << ' ' << static_cast<std::uint32_t>(left) << '-' << static_cast<std::uint32_t>(right);
            }
            out << '\n';
            for (const auto& sector : campaign.world().sectors()) {
                const auto& state = campaign.sector_state(sector.id);
                if (compact && state.control != ControlState::contested && state.control != ControlState::unstable) {
                    continue;
                }
                out << "Sector " << static_cast<std::uint32_t>(sector.id) << ' '
                    << faction_name(state.controller) << " | " << control_name(state.control) << '\n';
            }
        }
        for (const auto& [id, state] : campaign.combat_units()) {
            const auto& unit = campaign.units().at(id);
            if (perspective != FactionId::neutral && perspective != unit.faction) {
                continue;
            }
            out << unit.name << " | sector " << static_cast<std::uint32_t>(unit.sector)
                << " | " << ground_status_name(state.status) << " | morale " << state.morale
                << " | cohesion " << state.cohesion << " | fatigue " << state.fatigue
                << " | suppression " << state.suppression;
            if (const auto movement = campaign.ground_movement(id, perspective != FactionId::neutral)) {
                report_movement(out, *movement);
            }
            out << '\n';
        }
    }
    for (const auto faction : {FactionId::west, FactionId::east}) {
        if (perspective != FactionId::neutral && perspective != faction) {
            continue;
        }
        const auto view = campaign.faction_view(faction, false);
        if (view.strategy) {
            report_strategy(out, faction, *view.strategy);
        }
        out << faction_name(faction) << " intelligence | " << view.sectors.size() << " reports | "
            << campaign.world().sectors().size() - view.sectors.size() << " unknown sectors | "
            << view.installations.size() << " known installation sites\n";
        for (const auto& [sector, info] : view.sectors) {
            if (!compact) {
                out << "  Sector " << static_cast<std::uint32_t>(sector) << " last reported "
                    << faction_name(info.estimated_controller) << " at tick " << info.observed_tick
                    << " (" << confidence_name(info.confidence) << ")\n";
            }
        }
        const auto visible_contacts = compact ? std::min<std::size_t>(view.contacts.size(), 2) : view.contacts.size();
        for (std::size_t i = 0; i < visible_contacts; ++i) {
            const auto& info = view.contacts[i];
            out << "  Contact " << static_cast<std::uint32_t>(info.id) << ' ' << info.classification
                << " at sector " << static_cast<std::uint32_t>(info.estimated_sector)
                << " | " << confidence_name(info.confidence) << ' ' << info.confidence << "/1000"
                << " | seen tick " << info.observed_tick << '\n';
        }
        const auto first = compact && view.missions.size() > 2 ? view.missions.size() - 2 : 0;
        for (std::size_t i = first; i < view.missions.size(); ++i) {
            const auto& mission = view.missions[i];
            out << "  Mission " << static_cast<std::uint32_t>(mission.id) << ' '
                << mission_name(mission.kind) << " to sector " << static_cast<std::uint32_t>(mission.target)
                << " | " << mission_status_name(mission.status)
                << " | equipment " << static_cast<std::uint32_t>(mission.equipment);
            if (mission.status == MissionStatus::scheduled || mission.status == MissionStatus::preparing ||
                mission.status == MissionStatus::in_progress) {
                out << ' ' << equipment_status_name(campaign.equipment().at(mission.equipment).status);
            } else {
                out << " (historical assignment)";
            }
            if (mission.status == MissionStatus::completed && mission.kind == MissionKind::recon) {
                out << " | " << mission.detections << " detected";
            }
            out << '\n';
        }
    }
}

template<class Output>
void drain_operations(const CampaignState& campaign, FactionId perspective, std::size_t& next,
                      std::size_t& next_battle, std::size_t& next_shipment, bool logistics, Output output)
{
    FactionView view;
    auto events = campaign.observer_operations_log();
    if (perspective != FactionId::neutral) {
        view = campaign.faction_view(perspective);
        events = view.events;
    }
    const auto battles = perspective == FactionId::neutral ?
        campaign.observer_battle_log() : std::span<const BattleEvent>{};
    const auto shipments = logistics && perspective == FactionId::neutral ?
        campaign.logistics_log() : std::span<const LogisticsEvent>{};
    while (next < events.size() || next_battle < battles.size() || next_shipment < shipments.size()) {
        if (next_shipment < shipments.size() &&
            (next == events.size() || shipments[next_shipment].sequence < events[next].sequence) &&
            (next_battle == battles.size() || shipments[next_shipment].sequence < battles[next_battle].sequence)) {
            output(format_logistics_event(shipments[next_shipment++], campaign.clock().tick_seconds()));
            continue;
        }
        if (next_battle < battles.size() &&
            (next == events.size() || battles[next_battle].sequence < events[next].sequence) &&
            (next_shipment == shipments.size() || battles[next_battle].sequence < shipments[next_shipment].sequence)) {
            const auto& battle = battles[next_battle++];
            std::ostringstream line;
            line << "Event tick " << battle.tick << " (" << battle.tick * campaign.clock().tick_seconds()
                 << "s): sector " << static_cast<std::uint32_t>(battle.sector) << ' ' << battle.reason;
            if (battle.unit) {
                line << " | unit " << static_cast<std::uint32_t>(*battle.unit);
            }
            if (battle.amount) {
                line << " | value " << battle.amount;
            }
            output(line.str());
            continue;
        }
        const auto& entry = events[next++];
        std::ostringstream line;
        line << "Event tick " << entry.tick << " (" << entry.tick * campaign.clock().tick_seconds()
             << "s): " << faction_name(entry.faction) << " sector " << static_cast<std::uint32_t>(entry.sector);
        if (entry.mission) {
            line << " mission " << static_cast<std::uint32_t>(*entry.mission);
        }
        line << ' ' << entry.reason;
        output(line.str());
    }
}

std::string dashboard(const CampaignState& campaign, std::uint32_t speed,
                      const std::deque<std::string>& recent_events, std::optional<FactionId> perspective)
{
    std::ostringstream frame;
    frame << "OpenWar | Ctrl+C to stop\n"
          << "Tick: " << campaign.clock().tick()
          << " | Elapsed seconds: " << campaign.clock().elapsed_seconds()
          << " | Speed: " << speed << "x\n";

    if (perspective) {
        report_operations(frame, campaign, *perspective, true);
        for (const auto& event : recent_events) {
            frame << event << '\n';
        }
        return frame.str();
    }

    std::array<std::size_t, 3> sectors{};
    for (const auto& sector : campaign.world().sectors()) {
        ++sectors.at(static_cast<std::size_t>(campaign.sector_state(sector.id).controller));
    }
    frame << "Sectors | WEST: " << sectors[1] << " | EAST: " << sectors[2]
          << " | NEUTRAL: " << sectors[0] << '\n';

    report_forces(frame, campaign, true);
    if (!campaign.units().empty()) {
        for (const auto& event : recent_events) {
            frame << event << '\n';
        }
    }
    for (const auto& [id, stock] : campaign.stockpiles()) {
        frame << "Stockpile " << static_cast<std::uint32_t>(id) << " (" << faction_name(stock.owner) << ")\n";
        for (std::size_t i = 0; i < stock.quantity.size(); ++i) {
            if (stock.capacity[i]) {
                frame << "  " << resource_name(static_cast<Resource>(i)) << ": "
                      << stock.quantity[i] << '/' << stock.capacity[i];
                if (stock.reserved[i]) {
                    frame << " (" << stock.reserved[i] << " reserved)";
                }
                frame << '\n';
            }
        }
    }
    for (const auto& state : campaign.installations()) {
        const auto& plant = state.installation;
        frame << plant.name << " | work " << state.work << '/'
              << std::uint64_t{plant.recipe.intervals_per_batch} * 100
              << " | integrity " << plant.integrity << "% | workers " << plant.workers
              << " | power " << (plant.has_power ? "on" : "off") << '\n';
    }
    for (const auto& [id, shipment] : campaign.shipments()) {
        frame << "Shipment " << static_cast<std::uint32_t>(id) << ": "
              << shipment_status_name(shipment.status) << ", cargo on carrier: " << shipment.cargo << '\n';
    }
    if (campaign.units().empty()) {
        for (const auto& event : recent_events) {
            frame << event << '\n';
        }
    }
    return frame.str();
}

}  // namespace

void report_forces(std::ostream& out, const CampaignState& campaign, bool compact)
{
    for (const auto& [id, unit] : campaign.units()) {
        const auto readiness = campaign.unit_readiness(id);
        out << faction_name(unit.faction) << ' ' << unit.name << " | sector "
            << static_cast<std::uint32_t>(unit.sector) << " | ready "
            << readiness.ready << '/' << readiness.active << '\n';
        for (const auto& [item_id, item] : campaign.equipment()) {
            if (item.assignment.unit != id || (compact && item.status == EquipmentStatus::available)) {
                continue;
            }
            const auto& type = campaign.equipment_types().at(item.assignment.type);
            out << "  Equipment " << static_cast<std::uint32_t>(item_id) << ' ' << type.name << ": "
                << equipment_status_name(item.status);
            if (item.job) {
                out << " | " << service_name(item.job->kind) << ' ' << item.job->work_seconds
                    << '/' << service_seconds(type, item.job->kind) << 's';
            }
            if (item.damage) {
                out << " | damage " << item.damage << '%';
            }
            if (item.operating_seconds) {
                out << " | operated " << item.operating_seconds << 's';
            }
            if (item.reinforcement_eta) {
                out << " | ETA tick " << *item.reinforcement_eta << " from " << item.assignment.reserve_source;
            }
            out << '\n';
        }
    }
}

void print_events(const CampaignState& campaign)
{
    EventCursor{}.drain(campaign, [](const std::string& event) { std::cout << event << '\n'; });
}

void print_operations_events(const CampaignState& campaign, FactionId perspective, bool logistics)
{
    std::size_t next = 0;
    std::size_t next_battle = 0;
    std::size_t next_shipment = 0;
    drain_operations(campaign, perspective, next, next_battle, next_shipment, logistics,
                     [](const std::string& event) { std::cout << event << '\n'; });
}

void print_operations(const CampaignState& campaign, FactionId perspective, bool events)
{
    if (events) {
        print_operations_events(campaign, perspective);
    }
    std::cout << "Tick: " << campaign.clock().tick() << '\n'
              << "Elapsed seconds: " << campaign.clock().elapsed_seconds() << '\n';
    report_operations(std::cout, campaign, perspective, false);
}

void print_campaign_summary(std::ostream& out, const CampaignState& campaign, std::uint64_t seed)
{
    out << "Campaign summary | seed " << seed << '\n'
        << "Tick: " << campaign.clock().tick() << '\n'
        << "Elapsed seconds: " << campaign.clock().elapsed_seconds() << '\n';
    report_war(out, campaign);
    const auto battles = campaign.observer_battle_log();
    std::array<std::size_t, 3> gains{};
    std::array<std::size_t, 3> losses{};
    std::size_t intervals = 0;
    std::size_t exchanges = 0;
    bool fired = false;
    for (const auto& event : battles) {
        if (event.reason == "engagement") {
            ++intervals;
            fired = false;
        } else if (event.reason == "ammunition expended" && !fired) {
            ++exchanges;
            fired = true;
        }
        if (event.previous_controller) {
            ++gains.at(event.amount);
            ++losses.at(static_cast<std::size_t>(*event.previous_controller));
        }
    }
    out << "Engagement intervals: " << intervals << " | with ammunition expended " << exchanges << '\n';
    std::array<std::size_t, 3> territory{};
    for (const auto& sector : campaign.world().sectors()) {
        if (sector.properties.traversable) {
            ++territory.at(static_cast<std::size_t>(campaign.sector_state(sector.id).controller));
        }
    }
    for (auto faction : {FactionId::west, FactionId::east}) {
        const auto index = static_cast<std::size_t>(faction);
        UnitReadiness total;
        std::size_t reserve = 0;
        for (const auto& [id, item] : campaign.equipment()) {
            if (campaign.units().at(item.assignment.unit).faction != faction) {
                continue;
            }
            switch (item.status) {
            case EquipmentStatus::lost: ++total.lost; break;
            case EquipmentStatus::reserve:
            case EquipmentStatus::in_transit: ++reserve; break;
            case EquipmentStatus::available:
            case EquipmentStatus::ready: ++total.ready; [[fallthrough]];
            default: ++total.active; break;
            }
        }
        const auto view = campaign.faction_view(faction, false);
        if (view.strategy) {
            report_strategy(out, faction, *view.strategy);
        }
        const auto count = [&](MissionStatus status) {
            return std::ranges::count(view.missions, status, &Mission::status);
        };
        out << faction_name(faction) << " | controlled " << territory[index]
            << " | gained " << gains[index] << " | lost " << losses[index]
            << " | equipment " << total.ready << '/' << total.active << " ready"
            << " | reserve/in transit " << reserve << " | equipment lost " << total.lost
            << " | missions completed " << count(MissionStatus::completed)
            << " | failed " << count(MissionStatus::failed)
            << " | cancelled " << count(MissionStatus::cancelled) << '\n';
    }
    ResourceAmounts used{};
    for (const auto& entry : campaign.economy_ledger()) {
        if (!entry.shipment) {
            for (std::size_t resource = 0; resource < used.size(); ++resource) {
                used[resource] += entry.consumed[resource];
            }
        }
    }
    out << "Stockpile consumption";
    for (std::size_t resource = 0; resource < used.size(); ++resource) {
        if (used[resource]) {
            out << " | " << resource_name(static_cast<Resource>(resource)) << ' ' << used[resource];
        }
    }
    if (used == ResourceAmounts{}) {
        out << " | none";
    }
    out << '\n' << "Shipments delivered: " << std::ranges::count_if(campaign.shipments(), [](const auto& entry) {
        return entry.second.status == ShipmentStatus::delivered;
    }) << '/' << campaign.shipments().size() << '\n';
    for (const auto& [id, combat] : campaign.combat_units()) {
        if (const auto movement = campaign.ground_movement(id)) {
            const auto& unit = campaign.units().at(id);
            out << faction_name(unit.faction) << " " << unit.name << " | sector " << static_cast<std::uint32_t>(unit.sector);
            report_movement(out, *movement);
            out << '\n';
        }
    }
}

void run_realtime(CampaignState& campaign, std::uint32_t speed,
                  std::optional<std::uint64_t> tick_limit, bool events, std::optional<FactionId> perspective,
                  bool logistics)
{
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    const auto initial_tick = campaign.clock().tick();
    const auto tick_seconds = campaign.clock().tick_seconds();
    const auto maximum = std::numeric_limits<std::uint64_t>::max() / tick_seconds;
    const auto limit = tick_limit.value_or(maximum - initial_tick);
    if (limit > maximum - initial_tick) {
        throw std::overflow_error("Simulated time would overflow");
    }

    interrupted = 0;
    const InterruptHandler handler;
    const bool ansi = supports_ansi();
    {
        const TerminalScreen screen(ansi);
        EventCursor cursor;
        std::size_t next_operation = 0;
        std::size_t next_battle = 0;
        std::size_t next_shipment = 0;
        std::deque<std::string> recent_events;
        const auto draw = [&] {
            if (events) {
                const auto output = [&](std::string event) {
                    if (!ansi) {
                        std::cout << event << '\n';
                        return;
                    }
                    if (recent_events.size() == 5) {
                        recent_events.pop_front();
                    }
                    recent_events.push_back(std::move(event));
                };
                if (perspective) {
                    drain_operations(campaign, *perspective, next_operation, next_battle, next_shipment,
                                     logistics, output);
                } else {
                    cursor.drain(campaign, output);
                }
            }
            screen.draw(dashboard(campaign, speed, recent_events, perspective));
        };

        draw();
        const auto start = Clock::now();
        auto next_refresh = start + 100ms;
        auto displayed_tick = initial_tick;
        while (!interrupted && !campaign.war_ended() && campaign.clock().tick() - initial_tick < limit) {
            const auto now = Clock::now();
            const auto due = std::min(limit, ticks_due(now - start, speed, tick_seconds));
            const auto advanced = campaign.clock().tick() - initial_tick;
            if (due > advanced) {
                // Bound catch-up work so the CLI can observe an interrupt between batches.
                campaign.advance_ticks(std::min<std::uint64_t>(due - advanced, 256));
            }
            if (now >= next_refresh && campaign.clock().tick() != displayed_tick) {
                draw();
                displayed_tick = campaign.clock().tick();
                next_refresh = Clock::now() + 100ms;
            }
            if (campaign.clock().tick() - initial_tick == due && due < limit && !interrupted) {
                std::this_thread::sleep_for(50ms);
            }
        }
        if (displayed_tick != campaign.clock().tick()) {
            draw();
        }
    }
    if (interrupted) {
        std::cout << "Stopped at tick " << campaign.clock().tick() << " ("
                  << campaign.clock().elapsed_seconds() << " simulated seconds).\n";
    } else if (ansi && campaign.war_ended()) {
        report_war(std::cout, campaign);
    }
}

}  // namespace openwar::cli
