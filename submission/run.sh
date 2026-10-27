#!/usr/bin/env bash
set -euo pipefail

SUBMISSION_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# Python ZIP extraction may drop mode bits, and ingestion may skip build.sh.
if [[ ! -x "$SUBMISSION_ROOT/router" ]]; then
    chmod u+x -- "$SUBMISSION_ROOT/router"
fi
export LD_LIBRARY_PATH="$SUBMISSION_ROOT/lib:$SUBMISSION_ROOT/runtime/official-flute${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Keep the caller's directory so relative input and output paths retain meaning.
exec "$SUBMISSION_ROOT/router" \
    --model official \
    --official-runtime "$SUBMISSION_ROOT/runtime/official-flute" \
    --score-profile 20261020 \
    --tsv-resource-mode global-grid \
    --tsv-assignment-polish 1 --tsv-polish-heat-weight 2 \
    --tsv-polish-candidates 32 --tsv-polish-passes 8 --tsv-polish-bidirectional 1 \
    --official-score-pricing 1 --rrr-score-aware 1 --rrr-net-limit 512 \
    "$@"
