# openwar
Dynamic war simulator that will work as the basis for further simulation projects

## Build and test

Use a C++20 compiler (GCC or Clang), GNU Autoconf, GNU Automake, GNU Make,
and a POSIX shell. On Windows, use a native Windows toolchain and POSIX shell
such as those provided by MSYS2. On FreeBSD, use `gmake` for the commands below.

From a Git checkout:

```sh
autoreconf -fi
./configure
make
make check
./openwar --seed 42 --ticks 5
./openwar-gui
```

The default build includes `openwar-gui` and requires raylib. For a CLI-only
build, run `./configure --disable-viewer` before `make`.

Set `CXX=clang++` when running `configure` to select Clang. Use
`./configure --prefix=/your/install/path` to choose where `make install`
will install the executable. Use `make clean` to remove build outputs.

## Campaign foundation

The command-line simulator starts a predefined 3×2 scenario and reports
elapsed simulated time and sector ownership. Batch reports include terrain
and neighbors. The default seed is 0 and each tick advances 60 simulated
seconds. Use `--ticks 0` to inspect the initial state.

`WorldDefinition` holds the grid and its static geography.
`ScenarioDefinition` supplies the initial ownership, objective values, and
tick duration. `CampaignState` owns the current sector state, clock, and a
seeded MT19937 stream. Each sector uses the ID `y * width + x` and has
orthogonal geometric neighbors, including across non-traversable terrain.

## Command-line usage

```sh
./openwar
./openwar --speed 600
./openwar --ticks 100
./openwar --demo maintenance
./openwar --demo logistics --ticks 100
./openwar --demo maintenance --speed 600 --ticks 12 --events
```

By default, the economy demonstration opens a real-time terminal dashboard
and runs until Ctrl+C. Use `--demo economy`, `--demo logistics`, or
`--demo maintenance` to select a scenario. `--speed N` accepts a positive
32-bit integer in simulated seconds per real second. The default of 60
advances one simulated minute each real second.

`--ticks N` runs as fast as possible and prints the final state. Combine it
with `--speed N` to pace the run in real time and stop at the same tick limit.
Add `--events` in either mode for timestamped campaign events. ANSI terminals
show the latest five events in the dashboard. Redirected output uses plain
text and prints each event once.

Existing scripts can still use the deprecated `--logistics-demo` and
`--maintenance-demo` aliases. The legacy `--realtime` flag still enables
pacing with a tick limit and uses the current default speed of 60.

## Economy

Run `./openwar --ticks 10` to see the first refinery and factory batches.
The example economy runs every five simulation ticks. Its oil field draws from
a finite reserve, and its refinery and factory convert local inputs into
fuel, parts, and supplies. Oil and fuel are measured in whole liters. Materials,
parts, and supplies use whole equivalent units.

Installations sharing a same-owner local stockpile run in ascending ID order.
Later installations can use earlier output within the same interval. Each
batch checks input availability and output capacity before advancing work.
Integrity scales work, and inadequate power, staff, or integrity pauses
production. Completed batches and direct stock consumption are recorded in
an in-memory ledger in application order.

## Logistics

Run `./openwar --demo logistics --ticks 6` to deliver 80 liters of fuel to
a second depot. Run `./openwar --demo logistics --ticks 16 --events` to see
another 40 liters delivered after the refinery replenishes the origin.
The scenario defines directed transport links separately from the sector grid.
Carriers use travel time to choose among usable routes and can take a cheaper
route when fuel is limited. Every leg costs fuel and uses a route slot.
Requests compete by descending priority, earliest deadline, then request ID.

An idle carrier can travel empty to a new pickup, including a return to its
previous pickup. Dispatch prefers a carrier already at the origin, then the
shortest feasible pickup trip. Carriers refuel from unreserved fuel at the
pickup depot when needed. Delivery leaves the vehicle at its arrival sector
until it has another assignment.

Loading reserves stock before moving it into a carrier. The destination
accepts only what fits and the carrier keeps the remainder. A blockage
pauses carriers on the affected link. Carriers at a sector can choose a
different route. Cancelling loaded cargo leaves it with the carrier.
The campaign records reservations, cargo movements, delays, and losses in
memory. Carrier availability, blockages, and losses use explicit inputs.
Produced supplies become available for dispatch on the following tick.

## Forces and maintenance

Run `./openwar --demo maintenance --ticks 12 --events` to follow a damaged
Apache from a parts shortage through a logistics delivery, repair, and
scheduled maintenance. The demo also shows EAST and WEST inventories and a
reserve Hind arriving at its designated entry. Try `--ticks 1` to inspect
the blocked job or `--speed 3600` to watch the cycle.

Scenarios define unit hierarchies, individual equipment, compatible
workshops, service profiles, and finite reserve equipment. Jobs queue by
request tick and equipment ID, with started jobs served first. Workshops
charge their local stockpile once at service start and share bays and
technicians. Repair clears damage, scheduled service resets wear since the
last service, and returning equipment undergoes inspection and preparation.
Service runs after logistics and economy each tick, so a local delivery can
unblock a job that tick. Operation inputs increment usage in simulated
seconds. The demo uses illustrative service times and costs.

Reserve equipment has an explicit source and becomes active only after its
authorized arrival tick and a friendly, operational entry. Permanent losses
retain their IDs in the inventory history.

## Intelligence and operations

```sh
./openwar --demo operations --ticks 16 --events
./openwar --demo operations --faction WEST --order recon:1 --ticks 12 --events
```

The operations observer shows separate WEST and EAST reports. Select a
faction to see its reports and issue orders on its behalf. Startup orders
accept `recon:SECTOR`, `patrol:SECTOR`, `defend:SECTOR`, and
`reposition:SECTOR`. Use `--cancel ID` after an order to cancel it while
it is scheduled. Patrols, defense, and repositioning require confirmed
friendly sectors. Repositioning moves a single-equipment unit to another
compatible friendly facility.

Every four demo ticks the planner ranks known objectives by priority and
the need to refresh uncertain intelligence. It tries the ranked options
against friendly availability, range, and depot stock. Missions reserve
equipment, preparation stock, and flight fuel. The workshop debits preparation
stock when work starts, and the mission debits flight fuel on launch. Returning
equipment undergoes inspection and turnaround.

Contact and territorial confidence decay independently. In the demo, a
contact's four-minute half-life can prompt another reconnaissance of a
high-priority sector while its controller report is still confirmed.
Finished missions show their historical equipment assignment. Active
missions show the equipment's current status.

A reconnaissance pass samples its target sector and reports on return.
Forest limits detection in the demo, and a mission can return without a
contact. Reports preserve the last observation and decay from its timestamp
using an integer table sampled at sixteenths of a half-life. Contact IDs
and mission IDs are local to each faction. The demo shortens half-lives to make
decay visible in a short run. Report publication is immediate on return.

## Abstract combat and territory

```sh
./openwar --demo combat --ticks 30 --events
./openwar --demo combat --faction WEST --order attack:1 --ticks 16
```

The observer displays battle events and the front derived from neighboring
sector owners. A faction view shows its own units and reports. Ground attacks
require confirmed territorial intelligence. Hostile sectors also require a
reported contact or a recent complete survey. Ground defense requires
confirmed friendly territory.
Movement follows orthogonal, traversable sectors through known friendly
routes. Each step spends fuel from the unit's deployed stockpile.

Co-located opponents exchange abstract effects using ready equipment,
condition, experience, morale, cohesion, fatigue, suppression, terrain,
and supplies.
Ammunition and supplies are debited from the deployed stockpile. Damage and
losses apply to existing equipment IDs. Withdrawal takes a movement step,
and recovery uses the existing maintenance system and local resources.

Capture requires an eligible ground assignment to maintain uncontested
occupation. Defaults are 30 simulated minutes for ordinary sectors and 120
for critical sectors. Captured installations keep their inventory and damage
and wait through stabilization. The combat demo uses shorter intervals and
illustrative force profiles. Either faction can win by holding the demo's
central objective for three simulated minutes.

Campaign endings use configured hold objectives, which can also require
working installations, confirmed capitulation, formal surrender, or a time
limit. Simultaneous outcomes produce a draw. A final result freezes the
campaign at its terminal tick.

## Long campaign demonstration

```sh
./openwar --demo campaign --seed 42 --ticks 3000
./openwar --demo campaign --seed 42 --ticks 1000 --events
./openwar-gui --demo campaign --seed 42
```

The 9×7 scenario has three river crossings, rear industrial sites, forward
workshops, and opposing armor and reconnaissance formations. Opening supply
orders let the logistics system move factory output, ammunition, and fuel
to the formations. Local resupply policies configure a threshold, target and
priority by resource. A policy triggers when unreserved stock plus outstanding
deliveries falls below the threshold. The new order is limited by physical
free capacity after accounting for those deliveries. Ground stores stop
generating demand when their formation is destroyed or surrenders. The
campaign holds additional vehicles in reserve.

Victory by objective requires holding all three crossings and an operational
enemy relay for two simulated hours. The normal capitulation rules also
apply. If neither side wins within 48 simulated hours, the time limit
produces a draw. Headless execution prints a summary at the end of the run.
It counts combat resolution intervals, ownership transfers, equipment and
mission outcomes, and stockpile consumption. Shipments are reported separately
because moving stock does not consume the cargo.

## Campaign viewer

With raylib available through `pkg-config`, the build commands above
create `openwar-gui` alongside `openwar` in the directory where you ran
`make`. An out-of-tree build places both executables in its build directory.
On Windows, the executable is named `openwar-gui.exe`.

For a raylib installation outside `pkg-config`, pass its include and link
flags through `RAYLIB_CFLAGS` and `RAYLIB_LIBS` when running `configure`.

The viewer starts paused for inspection. Click a sector for details, use
Space to pause or resume, and use `[` and `]` to change speed. Scroll over the
map to zoom or over a panel to see more sector markers or events. Pan with
the middle or right mouse button or the arrow keys. Press R to fit the map.

Use the perspective buttons or keys 1–3 to switch between Observer, WEST,
and EAST. Observer shows authoritative ownership, contested sectors, and
events. Faction views show their own units and the territory, contacts,
installations, and orders in their reports. The viewer runs the same
campaign engine in its own process and advances it in whole simulated ticks.

## Roadmap

1. M0 — Core. Clock, world, sectors, and factions.
2. M1 — Economy. Installations, resources, production, and consumption.
3. M2 — Logistics. Depots, transports, routes, and supply.
4. M3 — Forces and maintenance. Units, equipment, wear, and repairs.
5. M4 — Operations and intelligence. Reconnaissance, commands, and planning.
6. M5 — Combat and territory. Engagements, morale, capture, and campaign progression.
7. M6 — Strategic map. 2D visualization and player intervention.
8. M7 — Flight. 3D helicopters and tactical integration.

The roadmap above is the current milestone sequence for the technical annexes.

## Design documents

- [`ANNEXES_README.md`](docs/ANNEXES_README.md) indexes the technical annexes.
- [`DESIGN.md`](docs/DESIGN.md) describes the overall campaign concept.
- [`TERRITORY.md`](docs/TERRITORY.md) specifies territorial control and campaign state.
- [`ECONOMY.md`](docs/ECONOMY.md) describes resources, production, and stock accounting.
- [`LOGISTICS.md`](docs/LOGISTICS.md) describes transport and supply.
- [`MILITARY_FORCES.md`](docs/MILITARY_FORCES.md) covers equipment, organization, and reserves.
- [`MAINTENANCE.md`](docs/MAINTENANCE.md) covers readiness and workshop service.
- [`COMBAT.md`](docs/COMBAT.md) covers abstract engagements, losses, and withdrawal.
- [`SCENARIOS.md`](docs/SCENARIOS.md) covers predefined, geographic, and procedural scenarios.
- [`CAMPAIGN_SETUP.md`](docs/CAMPAIGN_SETUP.md) describes campaign creation and configuration.
