#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
ROUTER_BINARY="${EDA_ROUTER_BINARY:-$PROJECT_ROOT/build/bin/eda_router}"
# Reject frozen historical binaries; they use a different resource contract.
if ! "$ROUTER_BINARY" --help | grep -- 'documented-rules-v1' >/dev/null; then
    echo 'This preset requires a documented-rules-v1 router; rebuild on the evaluation server.' >&2
    exit 1
fi
OFFICIAL_RUNTIME="${EDA_OFFICIAL_RUNTIME:-$PROJECT_ROOT/output/runtime/official-flute}"

# V7 algorithm parameters with the documented resource contract.
# Historical frozen results used a different score/resource model.
# Keep the caller's cwd so relative input, output, and environment paths work.
# Last arguments override defaults. For stage ablations use --rrr-rounds 0
# or --tsv-rounds 0; their V7 budget flags remain configured.
exec "$ROUTER_BINARY" \
    --model official --score-profile 20261020 \
    --official-runtime "$OFFICIAL_RUNTIME" \
    --tsv-resource-mode global-grid \
    --tsv-assignment-polish 1 --tsv-polish-heat-weight 2 \
    --tsv-polish-candidates 32 --tsv-polish-passes 8 --tsv-polish-bidirectional 1 \
    --official-score-pricing 1 --rrr-score-aware 1 --rrr-net-limit 512 \
    --resource-profile documented --time-limit 1800 \
    --v7-rrr 1 --rrr-rounds 8 \
    --v7-rrr-astar-net-limit 32 --v7-rrr-astar-seconds 0.03 \
    --v7-tsv 1 --v7-tsv-nets 512 --v7-tsv-candidates 6 --v7-tsv-seconds 45 \
    --v7-cell-cost 1 --v7-topology 1 \
    "$@"
