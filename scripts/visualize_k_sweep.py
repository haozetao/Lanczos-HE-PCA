#!/usr/bin/env python3
"""K-sweep 明文 PCA 重建对比。

直接读 16x16 灰度数据集 (.bin)，做明文 PCA 后在不同 K 取值下重建若干样本，
让我们直观看到"取多少个主成分才足够还原原图"。

输出：
  outdir/k_sweep.png  原图 + 6 个 K 取值的重建网格
  outdir/k_sweep_evr.txt  各 K 取值的累计方差解释率 (EVR)
"""
import argparse
import struct
import sys
from pathlib import Path

import numpy as np


def load_dataset(path: Path):
    with open(path, "rb") as f:
        N, d = struct.unpack("<II", f.read(8))
        data = np.frombuffer(f.read(N * d * 8), dtype="<f8").reshape(N, d).astype(np.float64)
    return data


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--input", required=True, help="data/*.bin 灰度数据集")
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--samples", type=int, nargs="+", default=[0, 10, 20, 30],
                    help="要可视化的样本 index")
    ap.add_argument("--k-list", type=int, nargs="+",
                    default=[1, 2, 4, 8, 16, 32, 64],
                    help="对照的 K 取值")
    ap.add_argument("--normalize", action="store_true",
                    help="像素值 /=255 (跟 Client 一致)")
    ap.add_argument("--title", default="")
    args = ap.parse_args()

    src = Path(args.input)
    out = Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)

    X = load_dataset(src)
    if args.normalize:
        X = X / 255.0
    N, d = X.shape
    side = int(round(d ** 0.5))
    assert side * side == d, f"d={d} 不是平方数"

    mean = X.mean(axis=0)
    Xc = X - mean[None, :]
    C = (Xc.T @ Xc) / max(N - 1, 1)
    evals, V_all = np.linalg.eigh(C)
    evals = evals[::-1]
    V_all = V_all[:, ::-1]

    total_var = evals.sum()
    cumvar = np.cumsum(evals) / total_var
    pixel_max = 1.0 if X.max() <= 1.5 else 255.0

    k_list = sorted(set(args.k_list))
    samples = [s for s in args.samples if 0 <= s < N]

    # ─── 写 EVR 表 ───
    lines = [f"# {src.name}", f"# N={N}  d={d} ({side}x{side})", ""]
    lines.append("K     λ_K          λ_K/λ_1     EVR (累计方差解释率)")
    for k in k_list:
        if k < 1 or k > d:
            continue
        lk = evals[k - 1]
        lines.append(f"{k:4d}   {lk:10.6f}   {lk/evals[0]:.4f}      {cumvar[k-1]:.4f}")
    (out / "k_sweep_evr.txt").write_text("\n".join(lines))
    print((out / "k_sweep_evr.txt").read_text())

    # ─── 重建网格 ───
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("[warn] matplotlib 未装")
        return 0

    rows = len(samples)
    cols = 1 + len(k_list)
    fig, axes = plt.subplots(rows, cols, figsize=(cols * 1.4, rows * 1.6))
    if rows == 1:
        axes = axes[None, :]
    for r, s in enumerate(samples):
        x = X[s]
        for c, k in enumerate([0] + k_list):  # 0 表示原图
            ax = axes[r, c]
            if k == 0:
                img = x
                title = "Original" if r == 0 else None
            else:
                V_k = V_all[:, :k]
                xhat = mean + V_k @ (V_k.T @ (x - mean))
                img = xhat
                title = f"K={k}" if r == 0 else None
            img_disp = np.clip(img, 0, pixel_max) / max(pixel_max, 1e-12)
            ax.imshow(img_disp.reshape(side, side), cmap="gray", vmin=0, vmax=1,
                      interpolation="nearest")  # nearest 避免 matplotlib 默认插值掩盖差异
            ax.set_xticks([])
            ax.set_yticks([])
            if title:
                ax.set_title(title, fontsize=9)
            if c == 0:
                ax.set_ylabel(f"#{s}", fontsize=8)
            # K>0 时标 EVR + 该图 PSNR
            if k > 0:
                mse = float(((x - img) ** 2).mean())
                psnr = 10*np.log10(pixel_max**2 / max(mse, 1e-12))
                ax.set_xlabel(f"EVR={cumvar[k-1]*100:.1f}%\nPSNR={psnr:.1f}dB",
                              fontsize=6)
    if args.title:
        fig.suptitle(args.title, fontsize=11)
    fig.tight_layout(rect=[0, 0, 1, 0.97 if args.title else 1])
    fig.savefig(out / "k_sweep.png", dpi=150)
    print(f"[ok] {out / 'k_sweep.png'}")


if __name__ == "__main__":
    sys.exit(main() or 0)
