#!/bin/sh
set -eu

openwar=${OPENWAR:-./openwar}
output=$("$openwar" --seed 42 --ticks 5)
repeat=$("$openwar" --seed 42 --ticks 5)
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
printf '%s\n' "$help" | tr -d '\r' | grep -Fx 'Usage: openwar [--seed UINT32] [--ticks UINT64]'

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
