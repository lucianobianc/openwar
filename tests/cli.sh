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
printf '%s\n' "$help" | tr -d '\r' | grep -Fx 'Usage: openwar [--demo NAME] [--speed N] [--ticks N] [--events] [--seed N]'
printf '%s\n' "$help" | grep -F 'Select economy (default), logistics, maintenance, operations, or combat.'
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
