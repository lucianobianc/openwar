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

std::string_view logistics_event_name(LogisticsEventKind kind)
{
    switch (kind) {
    case LogisticsEventKind::requested: return "requested";
    case LogisticsEventKind::assigned: return "assigned";
    case LogisticsEventKind::loaded: return "loaded";
    case LogisticsEventKind::delayed: return "delayed";
    case LogisticsEventKind::unloaded: return "unloaded";
    case LogisticsEventKind::delivered: return "delivered";
    case LogisticsEventKind::cancelled: return "cancelled";
    case LogisticsEventKind::lost: return "lost";
    case LogisticsEventKind::arrived: return "arrived";
    }
    throw std::logic_error("Unknown logistics event");
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
                (logistics == transport.size() || equipment[forces].tick < transport[logistics].tick) &&
                (economy == transactions.size() || equipment[forces].tick < transactions[economy].tick)) {
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
                (economy == transactions.size() || transport[logistics].tick <= transactions[economy].tick)) {
                const auto& entry = transport[logistics++];
                event << "Event tick " << entry.tick << " (" << entry.tick * campaign.clock().tick_seconds()
                      << "s): shipment " << static_cast<std::uint32_t>(entry.shipment)
                      << ' ' << logistics_event_name(entry.kind) << " [" << entry.quantity << ']';
                if (entry.sector) {
                    event << " in sector " << static_cast<std::uint32_t>(*entry.sector);
                }
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

std::string dashboard(const CampaignState& campaign, std::uint32_t speed,
                      const std::deque<std::string>& recent_events)
{
    std::ostringstream frame;
    frame << "OpenWar | Ctrl+C to stop\n"
          << "Tick: " << campaign.clock().tick()
          << " | Elapsed seconds: " << campaign.clock().elapsed_seconds()
          << " | Speed: " << speed << "x\n";

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

void run_realtime(CampaignState& campaign, std::uint32_t speed,
                  std::optional<std::uint64_t> tick_limit, bool events)
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
        std::deque<std::string> recent_events;
        const auto draw = [&] {
            if (events) {
                cursor.drain(campaign, [&](std::string event) {
                    if (!ansi) {
                        std::cout << event << '\n';
                        return;
                    }
                    if (recent_events.size() == 5) {
                        recent_events.pop_front();
                    }
                    recent_events.push_back(std::move(event));
                });
            }
            screen.draw(dashboard(campaign, speed, recent_events));
        };

        draw();
        const auto start = Clock::now();
        auto next_refresh = start + 100ms;
        auto displayed_tick = initial_tick;
        while (!interrupted && campaign.clock().tick() - initial_tick < limit) {
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
    }
}

}  // namespace openwar::cli
