# HE-PCA Experiments

This directory stores reproducible experiment logs and CSV summaries.

## Dimension Sweep

Laptop-safe sweep:

```bash
bash scripts/run_dimension_sweep.sh
```

Image-scale sweep:

```bash
RUN_LARGE=1 bash scripts/run_dimension_sweep.sh
```

Useful overrides:

```bash
DIMS="256 512 784" \
DATASET_N=500 \
TRUE_RANK=50 \
NOISE_SIGMA=2.0 \
K=4 \
M_ITER=5 \
NEWTON_ITERS=2 \
PER_ITER_GUESS=1 \
bash scripts/run_dimension_sweep.sh
```

Results:

- raw logs: `experiments/logs/*.log`
- parsed CSV: `experiments/results/dimension_sweep.csv`

Current high-dimensional runs still use synthetic low-rank data. Real image
datasets should be downsampled to 16x16/28x28/32x32 first, then evaluated with
the same R2(X), R2(V), eigenvalue error, bootstrap count, runtime, and memory
metrics.
