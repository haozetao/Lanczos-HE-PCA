#!/usr/bin/env bash
# 串行跑 MNIST-train-60k 与 Fashion-MNIST-train-60k 的 d=256 K=4 m=12 Newton=4 HE 实验。
# 每个 ~20 min，全程不依赖 shell session（用 nohup 调用本脚本）。
set -u
cd "$(dirname "$0")/.."

mkdir -p experiments/viz experiments/logs

echo "[$(date '+%F %T')] START MNIST-train 60k" >> experiments/logs/run_full_progress.log
K=4 NEWTON_ITERS=4 \
DATASET_FILE=data/mnist_16x16_train_full.bin \
VIZ_DUMP_PATH=experiments/viz/mnist_train60k_d256_m12_newton4.bin \
VIZ_DUMP_N=16 \
./build/he_pca bootstrap 12 256 \
  > experiments/logs/he_mnist_d256_train_full_m12_newton4.log 2>&1
echo "[$(date '+%F %T')] DONE MNIST-train 60k exit=$?" >> experiments/logs/run_full_progress.log

echo "[$(date '+%F %T')] START Fashion-train 60k" >> experiments/logs/run_full_progress.log
K=4 NEWTON_ITERS=4 \
DATASET_FILE=data/fashion_16x16_full.bin \
VIZ_DUMP_PATH=experiments/viz/fashion_train60k_d256_m12_newton4.bin \
VIZ_DUMP_N=16 \
./build/he_pca bootstrap 12 256 \
  > experiments/logs/he_fashion_d256_train_full_m12_newton4.log 2>&1
echo "[$(date '+%F %T')] DONE Fashion-train 60k exit=$?" >> experiments/logs/run_full_progress.log

echo "[$(date '+%F %T')] ALL DONE" >> experiments/logs/run_full_progress.log
