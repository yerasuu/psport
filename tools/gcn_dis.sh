#!/usr/bin/env bash
# Builds tools/gcn_dis (out/gcn_dis) from the recompiler's decoder; run inside the build environment.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
mkdir -p out
g++ -std=c++23 -O1 -Igpu/shim -Igpu/shadps4 -Igpu tools/gcn_dis.cpp \
    gpu/shadps4/shader_recompiler/frontend/{decode,instruction,format}.cpp -lfmt -o out/gcn_dis
