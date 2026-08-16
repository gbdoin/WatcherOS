#!/bin/bash
# Build the headless sim_life invariant harness (portable bash 3.2).
# Needs main/tama_logic.c to exist (it is the unit under test).
set -e
cd "$(dirname "$0")"
cc -std=c99 -Wall -Wextra -o sim_life sim_life.c ../main/tama_logic.c -I ../main -lm
echo "built: $(pwd)/sim_life"
