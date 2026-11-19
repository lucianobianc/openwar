# Connecticut Valley geographic scenario

This 32×32 km region is centred on Brattleboro, Vermont, and the Connecticut
River. The projected extent is eastings 682000–714000 m and northings
4730000–4762000 m in WGS84 / UTM zone 18N (EPSG:32618). The resolved world uses
16×16 strategic sectors and a 160×160 terrain lattice. The imported terrain
samples range from approximately 56 to 607 m above the supplied datum.

All authored scenarios use `valley-world.owgeo`, the same standalone
physical world. `valley-demo.owscenario` reproduces the original deployment,
with WEST and EAST outposts at sectors 122 and 175. The WEST route crosses the
Connecticut River and uses both roads and terrain. The original demonstration
ships 12 units of supplies to each outpost. `valley-autonomous.owscenario`
swaps the territorial corridors, changes the forces and supplies, and lets
both factions plan ground operations.

```sh
valley=examples/connecticut-valley
./openwar --inspect-world "$valley/valley-world.owgeo"
./openwar --scenario "$valley/valley-autonomous.owscenario" --ticks 40 --events
./openwar --scenario "$valley/valley-demo.owscenario" \
    --faction WEST --order defend:122 --ticks 120 --events
./openwar-gui --scenario "$valley/valley-autonomous.owscenario"
```

The original combined `connecticut-valley.owgeo` is still accepted by
`--scenario`.

## Valley Junction campaign

**Connecticut Valley - Valley Junction** is a separate scenario for evaluating
autonomous operations and exercising the command interface. Two fictional task
forces deploy on opposite sides of a neutral belt. They contest the central
junction in sector 121 and two secondary junctions in sectors 105 and 137.
Both sides can win by securing and holding sector 121 for one simulated hour.
The campaign also uses the engine's command-collapse conditions and a four-hour
time limit.

| Force | Initial deployment and organization |
|---|---|
| WEST | Command in sector 120, rear depot in 119, recon platoon in 104 and support platoon in 136. Its armor company fields heavier tanks with support trucks. |
| EAST | Command in sector 122, rear depot in 123, recon platoon in 106 and support platoon in 138. Its mechanized company and larger scout inventory give it more light maneuver equipment. |
| Both | Six formations per faction under a task-force headquarters. Each side includes an armor company, a mixed mechanized company, a recon platoon, a support platoon, and a two-aircraft reconnaissance flight. |

The workshops and aviation facilities are fictional. The geographic site
catalog remains available for inspection. Deployment points use existing
imported road nodes, and the engine resolves the terrain and bridge geometry
during routing. Initial order previews to the central junction found a
4.28 km route using three imported bridge segments from WEST and a 0.19 km
route from EAST. EAST gets early access to the objective by design. The
carrier router found all eight initial supply legs under the initial
territorial restrictions.

Two carriers per faction share the finite lift on their depot networks.
Initial shipments move ammunition and supplies to mobile formations, materials
to the workshop, and fuel to the reconnaissance flight. Recurring
policies replenish selected mobile stores and facilities. Forward units share
limited workshop capacity. Workshops can make spare parts while their
materials last.

Open the scenario from **New campaign → Connecticut Valley → Valley Junction**.
Use **Observer** for autonomous play, or **WEST commander** or **EAST commander**
to issue orders. For a first manual order, select one of the maneuver
companies, choose **Attack**, and click the initially neutral central junction.
Use **Missions (M)** and the **Routes** layer to follow operations. Zoom in or
cycle overlapping markers to inspect formations that share a sector.

```sh
scenario=examples/connecticut-valley/valley-junction.owscenario
./openwar --scenario "$scenario" --ticks 240 --seed 42 --events --save valley-full.ows
./openwar --scenario "$scenario" --ticks 45 --seed 42 --save valley-mid.ows
./openwar --load valley-mid.ows --ticks 195 --save valley-resumed.ows
cmp valley-full.ows valley-resumed.ows
```

### Reference run

Seed 42 reached a WEST victory at tick 132, after 2 h 12 min of simulated
time. EAST captured the two secondary sectors at ticks 17 and 18. WEST took
the central sector at tick 54 and later captured both secondary sectors.
Both sides launched their first attack on the central objective at tick 4
and used the existing coordinated-strategy and aerial reconnaissance rules.

| Metric | WEST | EAST |
|---|---:|---:|
| Orders scheduled / launched | 16 / 16 | 11 / 10 |
| Orders completed / failed | 14 / 2 | 4 / 7 |
| Reconnaissance missions completed | 11 | 2 |
| Equipment lost / surviving but unavailable | 7 / 1 | 10 / 1 |
| Shipments delivered / cancelled | 4 / 2 | 6 / 2 |

The run recorded 14 engagement intervals with ammunition expended, 32 military
sector crossings, 42 carrier sector crossings and five sector captures. Ten of
16 shipments were delivered, four were cancelled and two remained open when the
campaign ended. The stockpile ledger recorded consumption of 118 L of fuel,
29 supply units, 29 ammunition units, 16 material units and two spare parts.
The carriers' combined whole-litre tank balances fell by another 34 L.
The headless GCC build on an AMD EPYC 9V74 Linux host took 45.2 s and used
approximately 50 MiB peak RSS, including event output and checkpoint writing.
Resuming at tick 45 produced a byte-identical final checkpoint. Seeds 0 and 7
produced the same winner, end tick, engagement count and delivery count.

EAST's opening reach gave it early territorial gains, and WEST's ground force
prevailed in all three measured runs. Reconnaissance flights are
abstract sector missions with a duration and fuel cost. Air-defense assets use
the existing equipment and ground-combat model. Hierarchy is displayed in
inspection, with orders assigned by the engine's current formation rules.
Initial WEST previews of both secondary junctions hit the geographic search
budget. Later movement reached both through the campaign's evolving control
and routing state.

## Valley Junction 48-hour experiment

Choose **Valley Junction (48-hour experiment)** under Connecticut Valley. The
scenario uses the same world and active starting equipment as Valley Junction, with
armor in the center, scouts in the north, and mechanized companies on the
southern approach. Sector 121 is an intermediate objective. The flanks lead
to opposing outposts, and the rear depots are eight-hour hold objectives.
Command collapse uses a six-hour confirmation window. The time-limit draw
occurs at 48 simulated hours.

Each tick represents five minutes. Planning runs every 30 simulated minutes
and combat resolves every 20. Mobilized formations draw from finite depot
stocks, with two carriers per side, recurring deliveries, and a local
workshop for each southern mechanized company. The scenario retains the
existing repair, withdrawal, and replacement rules. Eight finite replacement
items are assigned to the armor and mechanized companies, four per side.
Autonomous workshop returns use the ordinary Move order.

```sh
./openwar --scenario examples/connecticut-valley/valley-junction-48h.owscenario --seed 42 --ticks 576 --events
```

To vary doctrines, use **Campaign settings** in the viewer and start as
**Observer**. A checkpoint from the observer session can be resumed headlessly
with `openwar --load FILE --ticks 576 --events`.

### Baseline reference runs

All four autonomous runs drew by the time limit at 48:00. Resuming the
Balanced seed-42 run at hour 24 produced a byte-identical final checkpoint.
Times are elapsed simulated hours and minutes. An engagement is one sector resolution
interval. Quiet hours are one-hour bins with no firing, capture, or military
sector crossing. A return to action is a distinct formation that reorganized
and then fired or launched another ground order.

| Seed | WEST / EAST | First–last combat | Engagements | Captures | Equipment lost / repairs completed | Returns | Deliveries | Quiet hours / final quiet stretch |
|---|---|---|---:|---:|---:|---:|---:|---:|
| 7, 42 | Balanced / Balanced | 0:40–4:00 | 9 | 2 | 16 / 1 | 1 | 14 of 18 | 43 / 43:50 |
| 42 | Offensive / Defensive | 0:40–8:00 | 15 | 3 | 15 / 0 | 0 | 25 of 27 | 41 / 40:00 |
| 42 | Defensive / Offensive | 0:40–7:20 | 14 | 3 | 18 / 6 | 0 | 26 of 30 | 41 / 40:40 |

WEST took the southern junction after securing sector 121 in the
Offensive / Defensive run. In the opposite matchup, EAST reached and fought
in WEST's command sector after securing the junctions. Deployments and
supplies supported several operations, but active ground forces were
exhausted or held at garrisons over the first eight hours. Depots still had
ammunition at the time limit. The authored format provides starting
equipment, and the planner needs surviving eligible formations to exploit
later objectives.

### Workshop recovery comparison

The baseline is the 48-hour scenario from commit `d15a8f9`, before
workshop returns and authored replacement inventory. Both versions used
the same seeds, doctrines, and observer role. Returns below count distinct
formations that reorganized, received a replacement, or finished a repair
and then fired or launched an Attack or Defend order.

| Seed | WEST / EAST | Last combat, baseline → recovery | Captures | Repairs / replacements | Returns | Deliveries | Quiet hours |
|---|---|---|---|---|---|---|---|
| 7, 42 | Balanced / Balanced | 4:00 → 16:20 | 2 → 7 | 1/0 → 7/5 | 1 → 3 | 14/18 → 25/26 | 43 → 3 |
| 42 | Offensive / Defensive | 8:00 → 5:20 | 3 → 3 | 0/0 → 2/2 | 0 → 1 | 25/27 → 29/34 | 41 → 42 |
| 42 | Defensive / Offensive | 7:20 → 12:20 | 3 → 3 | 6/0 → 12/4 | 2 → 4 | 26/30 → 25/30 | 41 → 36 |

Balanced ended in EAST victory by capitulation at 19:40 after WEST lost
its command sector at 13:40. The two mixed-doctrine runs drew at 48:00.
Neither fought into day two. With Offensive EAST, all three EAST maneuver
formations were eventually destroyed. With Defensive EAST, its surviving
armor and mechanized companies remained on garrison duty while WEST
ended with a broken armor company and no ready scouts. Both mixed runs
still had ammunition in rear depots.

Stockpile consumption below excludes transfers between stockpiles and
fuel drawn directly from carrier tanks. Fuel is in litres. Other columns
are stock units. Values are baseline → recovery.

| WEST / EAST | Fuel | Materials | Parts | Supplies | Ammunition |
|---|---:|---:|---:|---:|---:|
| Balanced / Balanced | 77 → 270 | 68 → 50 | 1 → 7 | 23 → 41 | 23 → 41 |
| Offensive / Defensive | 340 → 452 | 68 → 68 | 0 → 2 | 27 → 18 | 27 → 18 |
| Defensive / Offensive | 322 → 339 | 68 → 68 | 6 → 12 | 32 → 43 | 32 → 43 |

### Maneuver allocation comparison

The baseline is the workshop-recovery build at `ac658f1`. The scenario
and all initial inventories are identical across this comparison. The
revised Defensive planner keeps one armed stationary formation at a quiet
priority site and can assign the surplus elsewhere. Threatened sites
retain their previous protection.

| Seed | WEST / EAST | Garrison reassignments | Last combat | Captures | Repairs / arrivals | Back in action | Quiet hours | Outcome |
|---|---|---:|---|---|---|---|---|---|
| 7, 42 | Balanced / Balanced | 0 → 0 | 16:20 → 16:20 | 7 → 7 | 7/5 → 7/5 | 3 → 3 | 3 → 3 | EAST capitulation victory at 19:40 in both |
| 42 | Offensive / Defensive | 0 → 1 | 5:20 → 3:20 | 3 → 3 | 2/2 → 3/6 | 1 → 0 | 42 → 43 | Time-limit draw at 48:00 in both |
| 42 | Defensive / Offensive | 0 → 1 | 12:20 → 16:20 | 3 → 7 | 12/4 → 7/5 | 4 → 3 | 36 → 3 | 48:00 draw → EAST capitulation victory at 19:40 |

At hour six, the numbers of idle, attack-ready WEST/EAST formations were
1/2 → 1/2 for Balanced/Balanced, 1/2 → 2/3 for Offensive/Defensive, and
1/1 → 1/2 for Defensive/Offensive. These counts apply equipment, supply,
mobility and commitment checks before garrison, strategic recovery and
route checks. The corresponding numbers with a feasible route to at
least one authored attack objective were 1/2 → 1/2, 0/2 → 0/3, and
1/1 → 1/2. A feasible route does not confer an assignment.

In the Offensive/Defensive baseline, EAST armor was retained at sector
122 with a feasible attack at hour six. EAST mechanized waited for a
replacement, and EAST scouts were destroyed. WEST armor was broken,
WEST scouts had no ready occupying equipment, and WEST mechanized had
no confirmed attack target at that sample. By hour 12, neither faction
had an authored objective that passed the attack target checks. At hour
47, the revised run had two WEST and three EAST attack-ready formations
but still no such objective. In the reverse baseline, WEST armor was
initially limited by faction recovery and its garrison. Its late attack
options disappeared, and WEST mechanized and scouts plus all three EAST
maneuver formations were destroyed. Headquarters and support platoons
lack occupying equipment for Attack and Defend assignments. The recon
flights use air orders, and at least one side continued reconnaissance
late in each baseline draw.

No run fought into the second day. Releasing redundant coverage changed
the opening and one outcome. The late draw still needs usable objectives
and routes from current faction reports, while capitulation ends the
other campaigns before a second day can begin.

### Strategic objective eligibility audit

The baseline is `c56d948`, with workshop recovery and garrison reallocation.
In the seed-42 Offensive/Defensive run at hour 47, each faction had eight
authored attack objectives. Both still had ready formations.

| Faction | Confirmed friendly | Territorial report below 750 | Enemy control confirmed, contact or complete survey missing |
|---|---|---|---|
| WEST | 120, 121 | 105, 106, 122, 123, 138 | 137 |
| EAST | — | 104, 105, 119, 120, 122, 136, 137 | 121 |

The same WEST breakdown held at hour 12. EAST then had six stale
territorial reports, an expired contact at 136, and the same missing
contact or survey at 121. None passed the target gate at those samples,
so route or formation assignment checks could not account for the late
target rejections. Earlier in the campaign, a formation could also fail
assignment after the target gate because of its current operation,
equipment, supplies, or a route unavailable under faction reports.

The existing planner derives intermediate staging sectors geometrically
toward its current focus. Routing and reports determine which are usable.
The follow-on pass can also use recent faction reports to identify an
enemy position outside the authored target list. It runs after authored
targets and staging sectors, and starts at most one ground operation.

| Seeds | WEST / EAST | Last combat | Engagement intervals with fire | Captures | Quiet hours | Outcome in both versions |
|---|---|---|---|---|---|---|
| 7, 42 | Balanced / Balanced | 16:20 → 16:20 | 18 → 18 | 7 → 7 | 3 → 3 | EAST by capitulation at 19:40 |
| 42 | Offensive / Defensive | 3:20 → 8:20 | 6 → 10 | 3 → 3 | 43 → 41 | 48:00 time-limit draw |
| 42 | Defensive / Offensive | 16:20 → 16:20 | 18 → 18 | 7 → 7 | 3 → 3 | EAST by capitulation at 19:40 |

EAST launched two ground surveys in the modified Offensive/Defensive run.
Both failed, and the last fighting was part of those contacts. No reported
follow-on Attack passed all the checks in these runs. At hour 47, four
formations were still attack-ready, two per side, compared with five
in the baseline. All 16 authored targets were still rejected. Per faction,
seven territorial reports were below the confidence threshold and the
remaining enemy objective lacked a verified contact or complete survey.

In the other matchups, the hour-12 route audit found zero WEST formations
available for an otherwise valid target at 121. EAST had two attack-ready
formations, but neither could reach eligible targets at 104 or 136 under
the existing route and fuel checks. No run fought into the second day.

## Source data and reproduction

- `valley.osm.pbf` is a fixed, filtered OSM extract obtained on 18 November 2026
  from the OSM map API. The rectangular source queries cover approximately
  72.79°W–72.36°W and 42.68°N–42.999°N. They were sorted, merged and filtered
  with osmium 1.18.0, retaining road and waterway topology, land-use features,
  and the aviation and transport catalog. **© OpenStreetMap contributors**,
  under ODbL 1.0. The OSM-derived content in the resolved world is also provided
  under ODbL 1.0. https://www.openstreetmap.org/copyright
- `valley-elevation.tif` is a cropped and 2× averaged copy of
  `Copernicus_DSM_COG_10_N42_00_W073_00_DEM.tif`. The GLO-30 source is a surface
  model with EGM2008 orthometric heights. **Copernicus DEM © DLR e.V. 2010–2014
  and © Airbus Defence and Space GmbH 2014–2018, provided under COPERNICUS by
  the European Union and ESA; all rights reserved.** The product's public
  licence applies to the source and derived elevation samples.
  https://copernicus-dem-30m.s3.amazonaws.com/readme.html

The fixed source inputs are bundled. The full source URLs, SHA-256 hashes,
projection, import settings and diagnostics are embedded in the resolved
world. Rebuild with GDAL 3.10.3 and osmium 1.18.0 for the checked-in output.

```sh
base=examples/connecticut-valley
python3 tools/import_geography.py --config "$base/valley-world.json" \
    --osm "$base/valley.osm.pbf" --dem "$base/valley-elevation.tif" \
    --output /tmp/valley-world.owgeo
cmp "$base/valley-world.owgeo" /tmp/valley-world.owgeo
```

Use `connecticut-valley.json` to reproduce the legacy combined demonstration.
