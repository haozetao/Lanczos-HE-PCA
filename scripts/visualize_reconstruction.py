#!/usr/bin/env python3
"""PCA 重建可视化。

读取 he_pca bootstrap 模式 VIZ_DUMP_PATH 导出的二进制文件，按以下数学公式
做"原图 / HE 重建 / 明文 EVD 重建"三联图，量化它们的逐像素 MSE。

文件格式 (little-endian)：
  uint32 d            # 像素维度 (= image_side * image_side)
  uint32 K            # 主成分个数
  uint32 N            # 样本数
  float64 mean[d]                   # 训练集逐像素均值 (中心化用)
  float64 V_he[d, K]   row-major    # 密文 PCA 得到的前 K 个主成分
  float64 V_true[d, K] row-major    # 明文 EVD 真值前 K 个主成分
  float64 X[N, d]      row-major    # 原始图像 (未归一化, 0..255 像素)

重建公式 (与 R²(X) 的计算严格一致)：
    z   = V_K^T · (x - mean)         # 投影到 K 维主成分坐标
    x_hat = mean + V_K · z           # 反投影回 d 维

也就是说：把一张图减去训练集均值得到偏差向量，把偏差投影到 K=4 个主成分
方向上得到 K 个系数，再用这 K 个主成分线性叠加把图重建出来。重建质量
完全取决于 V_K 是否真的张成了"承载方差最大的 K 维子空间"——这与 cos 误差
不直接挂钩，但是和 R²(X) gap 严格一一对应。

输出：
  outdir/grid_he.png    左→右：原图 / HE 重建 / 明文 EVD 重建
  outdir/components.png 前 K 个主成分本身可视化 (HE vs 明文)
  outdir/mse_table.txt  逐图 MSE / 整体平均 MSE / 像素值范围

依赖：numpy + matplotlib (绘图)。matplotlib 缺失时只输出 mse_table.txt。
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

import numpy as np


def load_viz(path: Path):
    with open(path, "rb") as f:
        d, K, N = struct.unpack("<III", f.read(12))
        mean = np.frombuffer(f.read(d * 8), dtype="<f8").astype(np.float64)
        V_he = np.frombuffer(f.read(d * K * 8), dtype="<f8").reshape(d, K).astype(np.float64)
        V_true = np.frombuffer(f.read(d * K * 8), dtype="<f8").reshape(d, K).astype(np.float64)
        X = np.frombuffer(f.read(N * d * 8), dtype="<f8").reshape(N, d).astype(np.float64)
    side = int(round(d ** 0.5))
    assert side * side == d, f"d={d} 不是完全平方数, 无法当作 {side}x{side} 图"
    return mean, V_he, V_true, X, side, K


def reconstruct(X: np.ndarray, mean: np.ndarray, V: np.ndarray) -> np.ndarray:
    """x_hat = mean + V · V^T · (x - mean), broadcast over N rows."""
    Xc = X - mean[None, :]
    Z = Xc @ V             # (N, K) 主成分坐标
    Xhat = mean[None, :] + Z @ V.T
    return Xhat


def per_image_mse(X: np.ndarray, Xhat: np.ndarray) -> np.ndarray:
    return ((X - Xhat) ** 2).mean(axis=1)


def normalize_for_display(img: np.ndarray, pixel_max: float) -> np.ndarray:
    """裁剪到 [0, pixel_max] 后归一化到 [0,1] 给 imshow。"""
    img = np.clip(img, 0.0, pixel_max)
    return img / pixel_max if pixel_max > 0 else img


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--input", required=True, help="VIZ_DUMP_PATH 写出的二进制")
    ap.add_argument("--outdir", required=True, help="图片输出目录")
    ap.add_argument("--max-rows", type=int, default=8,
                    help="可视化网格的样本行数 (默认 8)")
    ap.add_argument("--pixel-max", type=float, default=0.0,
                    help="像素最大值 (0 = 从 X 数据自动检测：max<=1.5 → 1.0；否则 255)")
    ap.add_argument("--title", default="",
                    help="网格图主标题 (例如 'MNIST-train 60k d=256 K=4')")
    args = ap.parse_args()

    src = Path(args.input)
    out = Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)

    mean, V_he, V_true, X, side, K = load_viz(src)
    N = X.shape[0]
    rows = min(args.max_rows, N)

    pixel_max = args.pixel_max
    if pixel_max <= 0:
        pixel_max = 1.0 if X.max() <= 1.5 else 255.0
        print(f"[auto] pixel_max = {pixel_max:.1f}  (X.max() = {X.max():.3f})")

    Xhat_he   = reconstruct(X, mean, V_he)
    Xhat_true = reconstruct(X, mean, V_true)

    mse_he   = per_image_mse(X, Xhat_he)
    mse_true = per_image_mse(X, Xhat_true)

    table_lines = []
    table_lines.append(f"# {src.name}")
    table_lines.append(f"# d={side*side} ({side}x{side}), K={K}, N_dump={N}")
    table_lines.append(f"# 重建公式: x_hat = mean + V_K · V_K^T · (x - mean)")
    table_lines.append(f"# 像素范围: 0..{pixel_max:.1f}")
    table_lines.append("")
    table_lines.append("idx   MSE(HE)        MSE(EVD)       MSE diff")
    for i in range(N):
        table_lines.append(
            f"{i:3d}   {mse_he[i]:11.4f}    {mse_true[i]:11.4f}    "
            f"{mse_he[i] - mse_true[i]:+10.4f}"
        )
    table_lines.append("")
    table_lines.append(f"mean MSE(HE)  = {mse_he.mean():.4f}")
    table_lines.append(f"mean MSE(EVD) = {mse_true.mean():.4f}")
    table_lines.append(f"MSE gap (HE − EVD) = {mse_he.mean() - mse_true.mean():+.4f}")
    table_lines.append(f"PSNR(HE)  ≈ {10*np.log10(pixel_max**2 / max(mse_he.mean(),1e-12)):.2f} dB")
    table_lines.append(f"PSNR(EVD) ≈ {10*np.log10(pixel_max**2 / max(mse_true.mean(),1e-12)):.2f} dB")
    (out / "mse_table.txt").write_text("\n".join(table_lines))
    print((out / "mse_table.txt").read_text())

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("[warn] matplotlib 未安装, 已写 mse_table.txt 但不画图")
        return 0

    # ─── 三联图: 原图 / HE 重建 / EVD 重建 ───
    fig, axes = plt.subplots(rows, 3, figsize=(6, rows * 1.7))
    if rows == 1:
        axes = axes[None, :]
    col_titles = ["Original", f"HE Reconstruction (K={K})", f"Plaintext EVD (K={K})"]
    for i in range(rows):
        imgs = [
            X[i].reshape(side, side),
            Xhat_he[i].reshape(side, side),
            Xhat_true[i].reshape(side, side),
        ]
        for c, (img, t) in enumerate(zip(imgs, col_titles)):
            ax = axes[i, c]
            ax.imshow(normalize_for_display(img, pixel_max),
                      cmap="gray", vmin=0, vmax=1)
            ax.set_xticks([])
            ax.set_yticks([])
            if i == 0:
                ax.set_title(t, fontsize=10)
            if c == 0:
                ax.set_ylabel(f"#{i}", fontsize=8)
        # 在每行第 2、3 列下方标 MSE
        psnr_he = 10*np.log10(pixel_max**2 / max(mse_he[i],1e-12))
        psnr_evd = 10*np.log10(pixel_max**2 / max(mse_true[i],1e-12))
        axes[i, 1].set_xlabel(f"MSE={mse_he[i]:.4f}  PSNR={psnr_he:.1f}dB", fontsize=7)
        axes[i, 2].set_xlabel(f"MSE={mse_true[i]:.4f}  PSNR={psnr_evd:.1f}dB", fontsize=7)
    if args.title:
        fig.suptitle(args.title, fontsize=11)
    fig.tight_layout(rect=[0, 0, 1, 0.97 if args.title else 1])
    fig.savefig(out / "grid_he.png", dpi=150)
    plt.close(fig)
    print(f"[ok] {out / 'grid_he.png'}")

    # ─── 主成分本身的可视化 (HE vs 明文 EVD) ───
    # 主成分是 d 维向量, 把它 reshape 成 side×side 当成"特征脸"看
    fig, axes = plt.subplots(2, K, figsize=(K * 1.7, 3.5))
    if K == 1:
        axes = axes.reshape(2, 1)
    for k in range(K):
        for row, (V, name) in enumerate([(V_he, "HE"), (V_true, "EVD")]):
            ax = axes[row, k]
            comp = V[:, k].reshape(side, side)
            # 主成分本身有正负, 用 RdBu 突出方向
            vmax = float(np.abs(comp).max()) + 1e-12
            ax.imshow(comp, cmap="RdBu_r", vmin=-vmax, vmax=vmax)
            ax.set_xticks([])
            ax.set_yticks([])
            if row == 0:
                ax.set_title(f"PC{k+1}", fontsize=9)
            if k == 0:
                ax.set_ylabel(name, fontsize=9)
    if args.title:
        fig.suptitle(f"{args.title} — Principal Components", fontsize=10)
    fig.tight_layout(rect=[0, 0, 1, 0.92 if args.title else 1])
    fig.savefig(out / "components.png", dpi=150)
    plt.close(fig)
    print(f"[ok] {out / 'components.png'}")

    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
