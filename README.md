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
```

Set `CXX=clang++` when running `configure` to select Clang. Use
`./configure --prefix=/your/install/path` to choose where `make install`
will install the executable. Use `make clean` to remove build outputs.

## Campaign foundation

The command-line simulator starts a predefined 3×2 scenario and reports
elapsed simulated time, sector ownership, terrain, and neighbors. With no
options, it uses seed 0 and advances one 60-second tick. Use `--ticks 0` to
inspect the initial state.

`WorldDefinition` holds the grid and its static geography.
`ScenarioDefinition` supplies the initial ownership, objective values, and
tick duration. `CampaignState` owns the current sector state, clock, and a
seeded MT19937 stream. Each sector uses the ID `y * width + x` and has
orthogonal geometric neighbors, including across non-traversable terrain.

## Real-time CLI

```sh
./openwar --realtime --speed 60 --events --logistics-demo
```

`--realtime` advances whole simulation ticks and refreshes the dashboard
until Ctrl+C. The default speed is `1`, or one simulated second per real
second. Use `--speed 60` for one simulated minute per real second or
`--speed 3600` for one simulated hour per real second. Speed accepts a
positive 32-bit integer and applies only in real-time mode.

`--ticks N` still runs a batch when used on its own. With `--realtime`, it
stops the dashboard after exactly `N` ticks. Add `--events` in either mode to
see timestamped economy and logistics events. ANSI terminals show the latest
five events in the dashboard. Redirected output uses plain text and prints
each event once.

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

Run `./openwar --logistics-demo --ticks 6` to deliver 80 liters of fuel to
a second depot. The scenario defines directed transport links separately
from the sector grid. Carriers take the shortest usable route, spend fuel
on each leg, and wait when all slots on a link are occupied. Requests compete
for local stock and carriers by descending
priority, earliest deadline, then request ID. Deadlines rank requests.

Loading reserves stock before moving it into a carrier. The destination
accepts only what fits and the carrier keeps the remainder. A blockage
pauses carriers on the affected link. Carriers at a sector can choose a
different route. Cancelling loaded cargo leaves it with the carrier.
The campaign records reservations, cargo movements, delays, and losses in
memory. Carrier availability, blockages, and losses use explicit inputs.
Produced supplies become available for dispatch on the following tick.

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
- [`SCENARIOS.md`](docs/SCENARIOS.md) covers predefined, geographic, and procedural scenarios.
- [`CAMPAIGN_SETUP.md`](docs/CAMPAIGN_SETUP.md) describes campaign creation and configuration.
