#!/bin/sh
set -eu

openwar=${OPENWAR:-./openwar}

(
    # The default has no tick limit. Observe live output before stopping the child.
    scratch=$(mktemp -d)
    "$openwar" >"$scratch/output" &
    pid=$!
    cleanup() {
        kill "$pid" 2>/dev/null || :
        wait "$pid" 2>/dev/null || :
        rm -f "$scratch/output"
        rmdir "$scratch"
    }
    trap cleanup 0
    remaining=10
    while ! grep -Eq '^Tick: [1-9][0-9]* \| Elapsed seconds: [0-9]+ \| Speed: 60x' "$scratch/output"; do
        kill -0 "$pid"
        test "$remaining" -gt 0
        remaining=$((remaining - 1))
        sleep 1
    done
    grep -F 'Tick: 0 | Elapsed seconds: 0 | Speed: 60x' "$scratch/output"
    grep -F 'Refinery | work ' "$scratch/output"
)

output=$("$openwar" --seed 42 --ticks 5)
repeat=$("$openwar" --demo economy --seed 42 --ticks 5)
test "$output" = "$repeat"
output=$(printf '%s' "$output" | tr -d '\r')
printf '%s\n' "$output" | grep -Fx 'Seed: 42'
printf '%s\n' "$output" | grep -Fx 'Tick: 5'
printf '%s\n' "$output" | grep -Fx 'Elapsed seconds: 300'
printf '%s\n' "$output" | grep -Fx 'World: 3x2, sector size 2000 m'
printf '%s\n' "$output" | grep -Fx 'Sector 0 (0,0) WEST plains, traversable: yes, objective points: 1, neighbors: 3 1'
printf '%s\n' "$output" | grep -Fx 'Sector 2 (2,0) EAST plains, traversable: yes, objective points: 1, neighbors: 5 1'
printf '%s\n' "$output" | grep -Fx 'Sector 4 (1,1) NEUTRAL water, traversable: no, objective points: 0, neighbors: 1 5 3'

output=$("$openwar" --seed 4294967295 --ticks 0)
output=$(printf '%s' "$output" | tr -d '\r')
printf '%s\n' "$output" | grep -Fx 'Seed: 4294967295'
printf '%s\n' "$output" | grep -Fx 'Elapsed seconds: 0'
printf '%s\n' "$output" | grep -Fx 'Economy transactions: 0'

output=$("$openwar" --ticks 10)
output=$(printf '%s' "$output" | tr -d '\r')
printf '%s\n' "$output" | grep -Fx '  fuel (L): 900/2400'
printf '%s\n' "$output" | grep -Fx '  supplies: 10/30'
printf '%s\n' "$output" | grep -Fx 'Economy transactions: 3'
help=$("$openwar" --help)
printf '%s\n' "$help" | grep -F 'Usage: openwar [--demo NAME | --scenario FILE | --load FILE]'
printf '%s\n' "$help" | grep -F -- '--save FILE'
printf '%s\n' "$help" | grep -F 'Select economy (default), logistics, maintenance, operations, combat, campaign, or procedural.'
if printf '%s\n' "$help" | grep -E -- '--(logistics-demo|maintenance-demo|realtime)'; then
    exit 1
fi
output=$("$openwar" --demo logistics --ticks 6)
legacy=$("$openwar" --logistics-demo --ticks 6)
test "$output" = "$legacy"
printf '%s\n' "$output" | tr -d '\r' | grep -Fx 'Shipment 1: delivered, cargo on carrier: 0'
printf '%s\n' "$output" | tr -d '\r' | grep -Fx '  fuel (L): 80/120'
output=$("$openwar" --demo logistics --ticks 16 --events)
output=$(printf '%s\n' "$output" | tr -d '\r')
printf '%s\n' "$output" | grep -Fx 'Event tick 11 (660s): shipment 2 assigned [40]'
printf '%s\n' "$output" | grep -Fx 'Event tick 15 (900s): shipment 2 arrived [40] in sector 3'
printf '%s\n' "$output" | grep -Fx 'Event tick 16 (960s): shipment 2 delivered [40]'
printf '%s\n' "$output" | grep -Fx '  fuel (L): 120/120'

batch=$("$openwar" --seed 42 --demo logistics --ticks 10 --events)
live=$("$openwar" --seed 42 --demo logistics --speed 3600 --ticks 10 --events)
printf '%s\n' "$live" | tr -d '\r' | grep -Fx 'Tick: 10 | Elapsed seconds: 600 | Speed: 3600x'
printf '%s\n' "$live" | tr -d '\r' | grep -Fx 'Shipment 1: delivered, cargo on carrier: 0'
test "$(printf '%s\n' "$live" | tr -d '\r' | sed -n 's/^Tick: \([0-9][0-9]*\) |.*/\1/p' | tail -n 1)" = 10
batch_events=$(printf '%s\n' "$batch" | grep '^Event tick ')
live_events=$(printf '%s\n' "$live" | grep '^Event tick ')
test "$batch_events" = "$live_events"
printf '%s\n' "$live_events" | tr -d '\r' | grep -Fx 'Event tick 6 (360s): shipment 1 delivered [80]'
output=$("$openwar" --realtime --ticks 0)
printf '%s\n' "$output" | tr -d '\r' | grep -Fx 'Tick: 0 | Elapsed seconds: 0 | Speed: 60x'
output=$("$openwar" --speed 600 --ticks 0)
printf '%s\n' "$output" | tr -d '\r' | grep -Fx 'Tick: 0 | Elapsed seconds: 0 | Speed: 600x'

reject()
{
    if "$openwar" "$@" >/dev/null 2>&1; then
        printf 'Unexpectedly accepted %s\n' "$*" >&2
        exit 1
    fi
}

reject --seed 4294967296
reject --ticks 18446744073709551616
reject --ticks 18446744073709551615
reject --ticks -1
reject --ticks 12abc
reject --ticks
reject --unknown
reject --speed 0
reject --speed -1
reject --speed 1.5
reject --speed 4294967296
reject --speed
reject --speed 600 --ticks 18446744073709551615
reject --demo
reject --demo unknown
reject --demo economy --demo maintenance
reject --demo logistics --maintenance-demo

service=$("$openwar" --demo maintenance --ticks 12 --events)
legacy=$("$openwar" --maintenance-demo --ticks 12 --events)
test "$service" = "$legacy"
service=$(printf '%s\n' "$service" | tr -d '\r')
printf '%s\n' "$service" | grep -Fx 'Event tick 1 (60s): equipment 1 waiting for resources (repair)'
printf '%s\n' "$service" | grep -Fx 'Event tick 6 (360s): stockpile 1 consumed stock for equipment 1 | spare parts -2/+0'
printf '%s\n' "$service" | grep -Fx 'Event tick 9 (540s): equipment 1 repair (repair) completed'
printf '%s\n' "$service" | grep -Fx 'Event tick 12 (720s): equipment 1 available'
printf '%s\n' "$service" | grep -Fx 'WEST West air squadron | sector 0 | ready 1/1'
printf '%s\n' "$service" | grep -Fx 'EAST East air squadron | sector 2 | ready 2/2'
paced=$("$openwar" --demo maintenance --speed 3600 --ticks 12 --events)
test "$(printf '%s\n' "$service" | grep '^Event tick ')" = "$(printf '%s\n' "$paced" | tr -d '\r' | grep '^Event tick ')"

operations=$("$openwar" --demo operations --ticks 16 --events)
printf '%s\n' "$operations" | grep -F 'WEST sector 2 mission 1 planner: refresh incomplete intelligence'
printf '%s\n' "$operations" | grep -F 'EAST sector 0 mission 1 planner: refresh incomplete intelligence'
printf '%s\n' "$operations" | grep -F 'ground at sector 2 | uncertain '
printf '%s\n' "$operations" | grep -F 'Mission 1 recon to sector 2 | completed | equipment 1 (historical assignment)'
printf '%s\n' "$operations" | grep -F 'Mission 2 recon to sector 2 | scheduled | equipment 1 reserved'
printf '%s\n' "$operations" | grep -F 'Mission 1 recon to sector 0 | completed | equipment 6 (historical assignment)'
printf '%s\n' "$operations" | grep -F 'Mission 2 recon to sector 0 | scheduled | equipment 6 reserved'
manual=$("$openwar" --demo operations --faction WEST --order recon:1 --ticks 6 --events)
printf '%s\n' "$manual" | grep -F 'observation reported: no contacts detected'
if printf '%s\n' "$manual" | grep -E 'EAST intelligence|\): EAST sector'; then
    exit 1
fi
cancelled=$("$openwar" --demo operations --faction WEST --order recon:2 --cancel 1 --ticks 0)
printf '%s\n' "$cancelled" | grep -F 'Mission 1 recon to sector 2 | cancelled'
reject --demo operations --order recon:2 --ticks 1
reject --demo operations --faction WEST --order defend:2 --ticks 1
reject --demo operations --faction NEUTRAL --ticks 1

combat=$("$openwar" --demo combat --seed 42 --ticks 30 --events)
printf '%s\n' "$combat" | grep -F 'WEST sector 1 mission 1 autonomous ground order scheduled'
printf '%s\n' "$combat" | grep -F 'EAST sector 1 mission 1 autonomous ground order scheduled'
printf '%s\n' "$combat" | grep -F 'sector 1 equipment lost'
printf '%s\n' "$combat" | grep -F 'sector 2 withdrawal completed'
printf '%s\n' "$combat" | grep -F 'sector 1 sector captured'
printf '%s\n' "$combat" | grep -E 'War: (WEST|EAST) victory at tick [0-9]+ \(scenario objective\)'
paced=$("$openwar" --demo combat --seed 42 --ticks 30 --speed 3600 --events)
test "$(printf '%s\n' "$combat" | grep '^Event tick ')" = "$(printf '%s\n' "$paced" | grep '^Event tick ')"
friendly=$("$openwar" --demo combat --faction WEST --order attack:1 --ticks 1 --events)
printf '%s\n' "$friendly" | grep -F 'Mission 1 attack to sector 1 | in progress'
if printf '%s\n' "$friendly" | grep -E 'East field group|EAST intelligence|^Front:'; then
    exit 1
fi
reject --demo combat --faction WEST --order attack:2 --ticks 1

explicit=$("$openwar" --demo combat --faction WEST --order attack:1:1 --ticks 0)
printf '%s\n' "$explicit" | grep -F 'Mission 1 attack to sector 1 | scheduled | equipment 1 reserved'
reject --demo combat --faction WEST --order attack:1:2 --ticks 0
reject --demo combat --faction WEST --order attack:1:4294967296 --ticks 0
reject --demo combat --faction WEST --order attack:1:1 --order attack:1:1 --ticks 0

campaign=$("$openwar" --demo campaign --seed 42 --ticks 3000 --events)
printf '%s\n' "$campaign" | grep -Fx 'Campaign summary | seed 42'
printf '%s\n' "$campaign" | grep -E '^War: (.* victory|draw) at tick [0-9]+ '
printf '%s\n' "$campaign" | grep -E '^Engagement intervals: [1-9][0-9]*'
printf '%s\n' "$campaign" | grep -E '^Shipments delivered: (29|[3-9][0-9]|[1-9][0-9][0-9]+)/'
printf '%s\n' "$campaign" | grep -m 1 -F 'travelling empty to pickup'
printf '%s\n' "$campaign" | grep -m 1 -F 'strategy: set campaign objective'
printf '%s\n' "$campaign" | grep -F 'WEST strategy |'
printf '%s\n' "$campaign" | grep -F 'EAST strategy |'
progress=$("$openwar" --demo campaign --seed 42 --ticks 12)
printf '%s\n' "$progress" | grep -m 1 -E 'next sector [0-9]+ \| 50% \| ETA 120s'

(
    scratch=$(mktemp -d)
    trap 'rm -f "$scratch"/setup.toml "$scratch"/invalid.toml; rmdir "$scratch"' 0
    cat >"$scratch/setup.toml" <<'EOF'
schema_version = 1
generator_version = 1
seed = 18446744073709551615 # full 64-bit seed
width = 13
height = 11
west_armor = 4
east_armor = 2
EOF
    preview=$("$openwar" --scenario "$scratch/setup.toml" --ticks 0)
    printf '%s\n' "$preview" | grep -Fx 'Campaign summary | seed 18446744073709551615'
    printf '%s\n' "$preview" | grep -Fx 'World: 13x11, sector size 2000 m'
    test "$(printf '%s\n' "$preview" | grep -Ec '^[.F~]{13}$')" = 11
    run=$("$openwar" --demo procedural --scenario "$scratch/setup.toml" --seed 42 --ticks 500 --events)
    repeat=$("$openwar" --scenario "$scratch/setup.toml" --seed 42 --ticks 500 --events)
    test "$run" = "$repeat"
    printf '%s\n' "$run" | grep -Fx 'Campaign summary | seed 42'
    reject --demo combat --scenario "$scratch/setup.toml" --ticks 0
    reject --demo procedural --seed 18446744073709551616 --ticks 0
    for invalid in 'width = 129' 'seed = -1' 'unknown = 1' 'generator_version = 2' 'schema_version = 1' 'width ='; do
        printf 'schema_version = 1\n%s\n' "$invalid" >"$scratch/invalid.toml"
        reject --scenario "$scratch/invalid.toml" --ticks 0
    done
    printf 'seed = 42\n' >"$scratch/invalid.toml"
    reject --scenario "$scratch/invalid.toml" --ticks 0
)

(
    scratch=$(mktemp -d)
    trap 'rm -f "$scratch"/*; rmdir "$scratch"' 0
    "$openwar" --demo logistics --seed 42 --ticks 3 --save "$scratch/mid.ows" --events >"$scratch/mid.txt"
    grep -F 'Shipment 1: en route, cargo on carrier: 80' "$scratch/mid.txt"
    "$openwar" --load "$scratch/mid.ows" --ticks 13 --save "$scratch/resumed.ows" --events >"$scratch/resumed.txt"
    "$openwar" --demo logistics --seed 42 --ticks 16 --save "$scratch/full.ows" --events >"$scratch/full.txt"
    cmp "$scratch/resumed.ows" "$scratch/full.ows"
    cmp "$scratch/resumed.txt" "$scratch/full.txt"

    "$openwar" --load "$scratch/mid.ows" --ticks 3 --speed 3600 --save "$scratch/mid.ows"
    "$openwar" --demo logistics --seed 42 --ticks 6 --save "$scratch/full.ows"
    cmp "$scratch/mid.ows" "$scratch/full.ows"

    cat >"$scratch/world.toml" <<'EOF'
schema_version = 1
seed = 18446744073709551615
width = 32
height = 16
EOF
    "$openwar" --scenario "$scratch/world.toml" --ticks 150 --save "$scratch/full.ows"
    "$openwar" --scenario "$scratch/world.toml" --ticks 47 --save "$scratch/mid.ows"
    rm "$scratch/world.toml"
    "$openwar" --load "$scratch/mid.ows" --ticks 103 --save "$scratch/resumed.ows" >"$scratch/resumed.txt"
    grep -F 'Campaign summary | seed 18446744073709551615' "$scratch/resumed.txt"
    cmp "$scratch/resumed.ows" "$scratch/full.ows"

    geographic=${OWGEO_FIXTURE:-examples/connecticut-valley/connecticut-valley.owgeo}
    "$openwar" --scenario "$geographic" --faction WEST --order defend:122 --ticks 2 --save "$scratch/mid.ows"
    "$openwar" --load "$scratch/mid.ows" --ticks 38 --save "$scratch/resumed.ows" --events >"$scratch/resumed.txt"
    "$openwar" --scenario "$geographic" --faction WEST --order defend:122 --ticks 40 --save "$scratch/full.ows"
    cmp "$scratch/resumed.ows" "$scratch/full.ows"
    grep -F 'Imported geographic world: 16x16 sectors' "$scratch/resumed.txt"
    grep -F 'ground objective secured' "$scratch/resumed.txt"
    grep -Fx 'Shipments delivered: 2/2' "$scratch/resumed.txt"
    "$openwar" --scenario "$geographic" --faction WEST --order defend:122 \
        --ticks 0 --save "$scratch/sector-order.ows"
    "$openwar" --scenario "$geographic" --faction WEST --ground-order defend:20266672,15133974,165151 \
        --ticks 0 --save "$scratch/point-order.ows"
    cmp "$scratch/sector-order.ows" "$scratch/point-order.ows"
    "$openwar" --scenario "$geographic" --faction WEST --ground-order defend:20266672,15133974,165151:1 \
        --ticks 0 --save "$scratch/explicit-point-order.ows"
    cmp "$scratch/point-order.ows" "$scratch/explicit-point-order.ows"
    reject --load "$scratch/point-order.ows" --faction EAST --cancel 1 --ticks 0
    reject --load "$scratch/point-order.ows" --cancel 1 --ticks 0
    reject --load "$scratch/point-order.ows" --faction WEST --ground-order defend:20266672,15133974 --ticks 0

    valley=${VALLEY_FIXTURES:-examples/connecticut-valley}
    world="$valley/valley-world.owgeo"
    "$openwar" --inspect-world "$world" | grep -F 'Geographic world: 16x16 sectors, 9342 road segments'
    reject --inspect-world "$geographic"
    reject --inspect-world "$world" --ticks 0
    reject --scenario "$world" --ticks 0

    "$openwar" --scenario "$geographic" --faction WEST --order defend:122 --ticks 40 --events >"$scratch/legacy.txt"
    "$openwar" --scenario "$valley/valley-demo.owscenario" --faction WEST --order defend:122 --ticks 40 --events >"$scratch/split.txt"
    cmp "$scratch/legacy.txt" "$scratch/split.txt"

    cp "$world" "$scratch/valley-world.owgeo"
    cp "$valley/valley-autonomous.owscenario" "$scratch/valley-autonomous.owscenario"
    autonomous="$scratch/valley-autonomous.owscenario"
    "$openwar" --scenario "$autonomous" --ticks 6 --seed 42 --save "$scratch/split-mid.ows"
    "$openwar" --scenario "$autonomous" --ticks 40 --seed 42 --events --save "$scratch/split-full.ows" >"$scratch/autonomous.txt"
    grep -E 'WEST sector 175 mission [0-9]+ ground objective secured' "$scratch/autonomous.txt"
    grep -E 'EAST sector 122 mission [0-9]+ ground objective secured' "$scratch/autonomous.txt"
    grep -Fx 'Shipments delivered: 2/2' "$scratch/autonomous.txt"
    cmp "$world" "$scratch/valley-world.owgeo"
    cmp "$valley/valley-autonomous.owscenario" "$autonomous"
    mv "$scratch/valley-world.owgeo" "$scratch/removed-world.owgeo"
    mv "$autonomous" "$scratch/removed.owscenario"
    "$openwar" --load "$scratch/split-mid.ows" --ticks 34 --save "$scratch/split-resumed.ows"
    cmp "$scratch/split-full.ows" "$scratch/split-resumed.ows"

    cat >"$scratch/minimal.owgeo" <<'EOF'
OWGEO 2
grid 1 1 2000 2000
provenance "{}"
tiles 0 0
waterways 0
nodes 0
roads 0
features 0
end
EOF
    "$openwar" --inspect-world "$scratch/minimal.owgeo" | grep -F '1x1 sectors, 0 road segments'
    bad_scenario="$scratch/invalid.owscenario"
    reject_scenario()
    {
        if "$openwar" --scenario "$bad_scenario" --ticks 0 >"$scratch/invalid.out" 2>"$scratch/invalid.err"; then
            printf 'Unexpectedly accepted invalid military scenario\n' >&2
            exit 1
        fi
        grep -F "$1" "$scratch/invalid.err"
    }
    printf 'OWSCENARIO 2\nworld "minimal.owgeo"\n' >"$bad_scenario"
    reject_scenario 'expected OWSCENARIO 1'
    printf 'OWSCENARIO 1\nworld "missing.owgeo"\n' >"$bad_scenario"
    reject_scenario 'cannot load scenario world'
    printf 'OWSCENARIO 1\nworld "minimal.owgeo"\npoint invalid 2000000 0 0\n' >"$bad_scenario"
    reject_scenario "geographic point 'invalid' is outside the world"
    printf 'OWSCENARIO 1\nworld "minimal.owgeo"\nunit 1 WEST missing 0 0 80 "Test"\n' >"$bad_scenario"
    reject_scenario "unknown geographic point 'missing'"
    printf 'OWSCENARIO 1\nworld "minimal.owgeo"\ncontrol 0 WEST 0\ncontrol 0 EAST 0\n' >"$bad_scenario"
    reject_scenario 'duplicate initial sector control'

    reject --load "$scratch/absent.ows" --ticks 0
    reject --load "$scratch/full.ows" --demo campaign
    reject --load "$scratch/full.ows" --scenario "$scratch/world.toml"
    reject --load "$scratch/full.ows" --seed 1
    reject --load "$scratch/full.ows" --load "$scratch/full.ows"
    reject --ticks 0 --save
    dd if="$scratch/full.ows" of="$scratch/short.ows" bs=1 count=160 2>/dev/null
    reject --load "$scratch/short.ows" --ticks 0
    cp "$scratch/full.ows" "$scratch/corrupt.ows"
    printf 'X' | dd of="$scratch/corrupt.ows" bs=1 seek=50 conv=notrunc 2>/dev/null
    reject --load "$scratch/corrupt.ows" --ticks 0
    cp "$scratch/full.ows" "$scratch/version.ows"
    printf '\177' | dd of="$scratch/version.ows" bs=1 seek=19 conv=notrunc 2>/dev/null
    reject --load "$scratch/version.ows" --ticks 0
)
