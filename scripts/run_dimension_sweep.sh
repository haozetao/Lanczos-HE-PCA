#!/usr/bin/env bash
set -euo pipefail

# Reproducible high-dimensional synthetic experiments.
# Default profile runs dimensions that are practical on a laptop.
# Set RUN_LARGE=1 to include image-scale 784/1024 runs.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

LOG_DIR="${LOG_DIR:-experiments/logs}"
mkdir -p "$LOG_DIR"

if [[ ! -x build/he_pca ]]; then
  echo "[build] build/he_pca not found; building first"
  bash build.sh
fi

if [[ "${RUN_LARGE:-0}" == "1" ]]; then
  DIMS=(${DIMS:-50 100 256 512 784 1024})
else
  DIMS=(${DIMS:-50 100 256})
fi

M_ITER="${M_ITER:-5}"
DATASET_N="${DATASET_N:-500}"
NOISE_SIGMA="${NOISE_SIGMA:-2.0}"
K="${K:-4}"
NEWTON_ITERS="${NEWTON_ITERS:-2}"
PER_ITER_GUESS="${PER_ITER_GUESS:-1}"

echo "dimension,m_iter,dataset_n,true_rank,noise_sigma,k,newton_iters,per_iter_guess,log_file"

for d in "${DIMS[@]}"; do
  # Keep the synthetic intrinsic rank moderate for stable PCA, but let it grow
  # enough to resemble image manifolds at higher ambient dimensions.
  if [[ -n "${TRUE_RANK:-}" ]]; then
    rank="$TRUE_RANK"
  elif (( d <= 100 )); then
    rank=20
  elif (( d <= 256 )); then
    rank=30
  else
    rank=50
  fi

  log="$LOG_DIR/d${d}_m${M_ITER}_rank${rank}_noise${NOISE_SIGMA}_k${K}.log"
  echo "$d,$M_ITER,$DATASET_N,$rank,$NOISE_SIGMA,$K,$NEWTON_ITERS,$PER_ITER_GUESS,$log"

  DATASET_N="$DATASET_N" \
  TRUE_RANK="$rank" \
  NOISE_SIGMA="$NOISE_SIGMA" \
  K="$K" \
  NEWTON_ITERS="$NEWTON_ITERS" \
  PER_ITER_GUESS="$PER_ITER_GUESS" \
  ./build/he_pca bootstrap "$M_ITER" "$d" > "$log" 2>&1
done

python3 scripts/summarize_experiment_logs.py "$LOG_DIR" > experiments/results/dimension_sweep.csv
echo "[done] summary written to experiments/results/dimension_sweep.csv"
