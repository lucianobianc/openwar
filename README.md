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
Carriers travel between metric access points. On authored maps, the geographic
router uses the carrier's mobility profile, road directions, grades, bridges,
and permitted off-road terrain. Road slots are shared by both directions and
remain committed for the tick in which a carrier exits. Requests compete by
descending priority, earliest deadline, then request ID.

An idle carrier can travel empty to a new pickup, including a return to its
previous pickup. Dispatch prefers a carrier already at the origin, then the
shortest feasible pickup trip. Carriers refuel from unreserved fuel at the
pickup depot when needed. Loading and unloading use their configured service
times and require the carrier to be at the exact access point. A formation's
mobile stockpile follows its current position. If its destination changes
during travel, the carrier updates the plan after its paid leg. Withdrawal
suspends service. If the formation is destroyed or surrenders, cancellation
releases any pickup reservation and leaves loaded cargo with its carrier.

Loading reserves stock before moving it into a carrier. The destination
accepts only what fits and the carrier keeps the remainder. At leg entry the
carrier pays fuel once, including any fractional litre. A closed road or
bridge can trigger an alternate route from the current position. A carrier
with no usable route waits with its cargo and progress. Shipments distinguish
route search limits, unreachable routes, fuel shortages, closures, hostile
territory, and full roads or destinations. Produced supplies become available
for dispatch on the following tick.

Legacy scenarios use their existing directed transport links as straight
geographic corridors, retaining each link's travel time, fuel cost, and slots.
Fixed stockpiles and starting carriers default to sector centres. Local
approaches connect these corridors to exact pickup and delivery points.

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
Ground formations and their mobile stockpiles change sectors when the
formation's geographic position crosses a boundary. Movement and withdrawal
advance along a journey in simulated time. Ground missions reach their exact
destination before completing. Sector presence continues to drive abstract
combat and occupation.

The C++ scenario definition accepts a geographic movement map, formation
positions, sector objective points, and equipment mobility profiles. On
these maps, routes can use roads, off-road terrain, or both. Planning uses
confirmed friendly reports for intermediate sectors and reports when a
bounded search cannot establish a route. Sector orders use a configured
objective point or the centre with sampled elevation. Use a configured
point for objectives with an inaccessible centre.

Geographic routing uses the fine grid for nearby endpoints. Long searches
and detours beyond the local window use the directed road graph and a
terrain lattice sampled at approximately sector resolution. Terrain edges
are checked against fine terrain, grades, barriers, and sector access before
they become executable legs. One budget applies across local and regional
attempts. A regional search can miss a passage between samples, in which
case it reports a search limit.

Existing demos and procedural scenarios derive straight geographic
corridors from their orthogonal sector links. The adapter preserves forest
travel and fuel multipliers, water restrictions, and the charge of one
fuel unit per 2 km of sector width, rounded up, times the terrain cost.
Ground movement pays fuel once at leg entry. Authored routes account for
millilitres, drawing whole litres from the formation's stores and carrying
forward the surplus. Convoys use the configured travel times and fuel
costs of transport links.

Co-located opponents exchange abstract effects using ready equipment,
condition, experience, morale, cohesion, fatigue, suppression, terrain,
and supplies.
Ammunition and supplies are debited from the deployed stockpile. Damage and
losses apply to existing equipment IDs. Withdrawal takes a movement step,
and recovery uses the existing maintenance system and local resources.

Capture requires an eligible ground assignment to maintain uncontested
occupation. Defaults are 30 simulated minutes for ordinary sectors and 120
for critical sectors. Captured installations keep their inventory and damage
and wait through stabilization. The combat demo uses 500 m sectors, shorter
intervals, and illustrative force profiles. Either faction can win by holding the demo's
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

### Advanced strategic command

In `--demo campaign`, each faction keeps a strategic objective between
reviews. Command revisits its priorities every 60 simulated ticks and when
it secures its focus or takes losses. Scenario victory targets receive extra
weight. If a direct advance is unavailable, command can survey and secure
approach sectors, or use available formations on another front.

The planner uses faction reports to identify threats. It guards threatened
command posts and objectives, and can synchronize two formations against
reported opposition by delaying the nearer formation. Existing movement,
stock, readiness and manual order checks still apply. After losses, command
can dispatch one existing replacement per review from a friendly functional
entry point. Heavy losses can pause new offensives until forces recover.
The demo prints each faction's current focus and review tick. Use `--events`
to see strategic decisions alongside operations and logistics.

## Procedural scenarios

```sh
./openwar --demo procedural --seed 2026 --ticks 3000 --events
./openwar --scenario examples/procedural.toml --ticks 0
./openwar --scenario examples/procedural.toml --seed 2026 --ticks 3000
./openwar-gui --scenario examples/procedural.toml
```

With `--ticks 0` the CLI validates and previews the map. `--scenario` accepts
a flat TOML subset with unsigned integers and `#` comments. Include
`schema_version = 1`. The optional `generator_version` must also be 1.
Omitted values use the defaults below. A command-line `--seed` overrides the
file. Procedural seeds are unsigned 64-bit values.

| Setting | Default | Accepted values |
| --- | --- | --- |
| `seed` | 0 | 0 through 2⁶⁴ − 1 |
| `width`, `height` | 9, 7 | Width 9–128 and height 5–128 |
| `sector_size_m` | 2000 | 500–5000 |
| `crossings` | 3 | 1–16, at most the map height. One armored formation per crossing on each side. |
| `forest_percent`, `water_percent` | 30, 10 | 0–100 each, with a combined maximum of 100 |
| `west_armor`, `east_armor` | 3, 3 | 1–8 active vehicles per formation |
| `reserve_armor`, `scout_flights` | 1, 2 | 0–4 reserve vehicles per formation and 1–4 flights per faction |
| `oil`, `materials`, `ammunition` | 600, 240, 90 | Starting industry stocks per faction. Minimums are 20, 8 and eight ammunition units per crossing. Maximum is 100,000 each. |
| `hold_minutes`, `time_limit_minutes` | 120, 2880 | Positive hold time. Time limit must allow for the first planning interval, one crossing leg, 120 minutes of occupation and the hold time. Maximum is 10,080 minutes. |
| `require_relay`, `capitulation` | 1, 1 | 0 or 1. Require the opposing relay in addition to the crossings, or enable victory through command collapse. |

The generator places crossings, rear industrial sites, airfields, workshops
and initial formations. It preserves connected ground and supply corridors,
and scales generated road travel time and fuel use with the sector size.
Even widths have two central river columns with connected crossing sectors.
Maps taller than 25 sectors distribute reconnaissance flights among
airfields along each supply corridor.
The terrain percentages apply to background cells before the generator
places the river and corridors and removes isolated land. The generated
scenario uses the same economy, logistics, strategic command and combat
engine as the fixed campaigns.

This version accepts procedural configurations and ships the existing fixed
scenarios as demos. Authored map packages and real-world map import remain
outside this generator.

### Large-world measurements

Headless GCC `-O2` runs on Linux with an AMD EPYC 9V74, using seed 42 and
600 simulated ticks. The sector width was 2 km. Each formation had five
armored vehicles and two reserves, and each faction had four reconnaissance
flights. Starting industry stocks were 2,000 oil, 800 materials and 800
ammunition per faction. Timings exclude generation and are approximate.

| Map | Formations and flights | Equipment including reserves | Carriers | Before | After | Peak RSS after |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 32×32 | 16 | 64 | 48 | 0.54 s | 0.18 s | 2.7 MiB |
| 64×64 | 24 | 120 | 80 | 4.03 s | 0.79 s | 3.7 MiB |
| 128×128 | 40 | 232 | 144 | 43.83 s | 4.22 s | 7.8 MiB |

Profiling the largest case identified ground-order planning and transport
routing as the largest CPU costs, followed by territory updates and faction
view construction. Routing now skips ground paths that exceed available fuel
and checks only the incoming links at each road junction. Strategy reuses
its faction view within a planning pass. The campaign event output matched
byte for byte before and after these changes at all three sizes.
The same 128×128 configuration ran 2,880 ticks in 20.7 seconds with
8.0 MiB peak RSS. These measurements describe the tested workload on this
machine. Campaigns with more equipment or more active fronts can cost more.

## Geographic scenarios

```sh
./openwar --scenario examples/connecticut-valley/connecticut-valley.owgeo --ticks 120 --events
```

The bundled 32×32 km Connecticut Valley scenario uses a fixed OpenStreetMap
extract and a Copernicus DEM. The terrain and roads are resolved offline. The
geographic catalog preserves typed site candidates independently of the
scenario's fictional installations. See the [import guide](docs/GEOGRAPHIC_IMPORT.md)
and [source attribution](examples/connecticut-valley/README.md).

## Saving and resuming campaigns

```sh
./openwar --demo campaign --seed 42 --ticks 600 --save campaign.ows
./openwar --load campaign.ows --ticks 400 --events --save campaign.ows
./openwar-gui --load campaign.ows --save updated.ows
```

`--ticks` advances the requested number of additional ticks or stops at the
campaign's conclusion. Use `--ticks 0` to inspect a checkpoint at its saved
tick. The CLI writes a checkpoint on normal completion, including a graceful
Ctrl+C in real-time mode. The viewer opens paused at the saved tick and
writes its checkpoint when the window closes. Use the same path for
`--load` and `--save` to update a checkpoint in place.

A checkpoint contains the resolved world and full simulation state,
including stocks and reservations, cargo in transit, service work,
reinforcement arrivals, orders and movement progress, faction intelligence,
the war result, the random generator state, and ordered event history.
Loading does not need the original scenario file or the procedural
generator. The save records the initial definition's fingerprint, the
scenario seed, and the generator version for procedural campaigns.

The binary format has independent schema and rules versions. Schema 4
stores resolved geographic provenance, waterways and site features along
with carrier and formation positions, active journeys, fuel accounting,
road commitments, and loading or unloading progress. When loading a version 1
or version 2 checkpoint, the carrier's former link and remaining travel ticks
reconstruct its position along a straight corridor and retain the fuel already
paid. Version 1 ground progress is converted along its former centre-to-centre
path. These conversions can change derived sector membership. Rules version 5 supports imported waterways and land cover.
A loaded journey retains its saved geometry and costs.
New route requests use the current routing rules. The loader checks file integrity
and supported versions before restoring state. The writer stages the complete
file beside the destination before replacing it. Checkpoints describe
simulation state at a tick boundary.
Viewer settings such as camera position, playback speed, and perspective
are local to each session.

## Campaign viewer

With raylib available through `pkg-config`, the build commands above
create `openwar-gui` alongside `openwar` in the directory where you ran
`make`. An out-of-tree build places both executables in its build directory.
On Windows, the executable is named `openwar-gui.exe`.

For a raylib installation outside `pkg-config`, pass its include and link
flags through `RAYLIB_CFLAGS` and `RAYLIB_LIBS` when running `configure`.

Open the bundled geographic scenario with
`./openwar-gui --scenario examples/connecticut-valley/connecticut-valley.owgeo`.
The map shows shaded elevation, roads and bridges, surface water, and
geographic sites. The layer buttons also control sector boundaries,
territory, formations, and carriers. Zoom in to see smaller geographic sites.
Legacy scenarios use their sector terrain and available campaign positions.

The viewer starts paused for inspection. Click a marker, road, or other
feature for details. Click again to cycle overlapping items, or click open
ground for sector reports. Space pauses or resumes and `[` and `]` change
playback speed. The wheel zooms around the cursor on the map and scrolls
reports in the side panels. Pan with the middle or right mouse button or
the arrow keys. Press R or click Fit to reset the view. Cursor coordinates
are metres east and south of the map's northwest origin. On imported maps,
the cursor's elevation is sampled from the terrain grid.

Use the perspective buttons or keys 1–3 to switch between Observer, WEST,
and EAST. Observer shows authoritative ownership, contested sectors, and
events. Faction views show their own formations, carriers and facilities,
along with the territory, contacts, installations, and orders in their
reports. Sector-level reports use approximate positions. Markers are simple
viewer glyphs pending the official APP-6 renderer. The viewer advances the
same campaign engine in whole simulated ticks and supports the CLI's
checkpoint format with `--load` and `--save`.

## Roadmap

1. M0 — Core. Clock, world, sectors, and factions.
2. M1 — Economy. Installations, resources, production, and consumption.
3. M2 — Logistics. Depots, transports, routes, and supply.
4. M3 — Forces and maintenance. Units, equipment, wear, and repairs.
5. M4 — Operations and intelligence. Reconnaissance, commands, and planning.
6. M5 — Combat and territory. Engagements, morale, capture, and campaign progression.
7. M6 — Advanced strategic command. Coordinate the war through long-term planning,
   strategic priorities, coordinated offensives, military reserves, defense,
   and adaptation to losses.
8. M7 — Scenarios and procedural campaigns. Generate maps and configure initial
   force deployments, infrastructure, resources, objectives, and victory conditions.
9. M8 — Persistence. Save and resume the full simulation state, including forces,
   economy, logistics, intelligence, and operations in progress.
10. M9 — Strategic interface. Replace the temporary viewer with an interactive
    map, military information, operation tracking, reports, and order issuance.
11. M10 — Integration and validation. Consolidate the strategic simulator through
    full-campaign tests, performance and determinism checks, scenario balancing,
    and integration fixes.

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
- [`GEOGRAPHIC_WORLD.md`](docs/GEOGRAPHIC_WORLD.md) specifies geographic coordinates, movement, and offline map import.
- [`COMMAND_INTERFACE.md`](docs/COMMAND_INTERFACE.md) specifies the 2D command interface, mission planning, and APP-6 symbology.
