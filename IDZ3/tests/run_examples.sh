#!/usr/bin/env bash
# Наборы из условия и небольшой сценарий для наглядного разбора.
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build-cmake
cmake --build build-cmake
mkdir -p results/examples
for scenario in demo day contention empty; do
    ./build-cmake/gallery --config "examples/$scenario.conf" \
        --log "results/examples/$scenario.log" --quiet \
        > "results/examples/$scenario-summary.txt"
done
echo "Примеры выполнены. Журналы и статистика: results/examples/"
