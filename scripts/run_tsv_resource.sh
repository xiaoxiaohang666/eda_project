#!/usr/bin/env bash
set -euo pipefail
PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# A1: the best uniform three-case configuration in both 900s and 1800s
# benchmark-host ablations. Later arguments may explicitly enable experimental
# landing quotes or targeted RRR; these are not part of the recommended preset.
exec bash "$PROJECT_ROOT/scripts/run_v9.sh" \
    --tsv-track-demand 1 --tsv-landing-pricing 0 --tsv-landing-rrr 0 \
    "$@"
