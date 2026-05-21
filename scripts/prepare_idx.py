#!/usr/bin/env python3
"""MNIST / Fashion-MNIST IDX 格式 → 16×16 灰度向量化二进制文件。

零依赖（仅 stdlib），输出格式与 prepare_yale.py 完全一致：
  uint32 N         # 样本数
  uint32 d         # 特征维度 (= TARGET*TARGET)
  float64 × N × d  # row-major X[N, d]，0..255 未归一化

Yann LeCun 镜像（如已无法访问，可换 https://ossci-datasets.s3.amazonaws.com/mnist/ 或
Fashion-MNIST 官方 https://github.com/zalandoresearch/fashion-mnist/raw/master/data/fashion）:
  train-images-idx3-ubyte.gz  /  train-labels-idx1-ubyte.gz
  t10k-images-idx3-ubyte.gz   /  t10k-labels-idx1-ubyte.gz
"""
import argparse
import gzip
import struct
import sys
from pathlib import Path


def open_maybe_gz(path: Path):
    """同时支持 .gz 与未解压文件。"""
    if path.suffix == ".gz":
        return gzip.open(path, "rb")
    return open(path, "rb")


def read_idx_images(path: Path):
    """读 IDX3-UBYTE 图像文件，返回 (N, h, w, raw_bytes_list)。"""
    with open_maybe_gz(path) as f:
        magic, n, h, w = struct.unpack(">IIII", f.read(16))
        if magic != 2051:
            raise ValueError(f"{path} 不是 IDX3 (magic={magic})")
        all_bytes = f.read(n * h * w)
        if len(all_bytes) != n * h * w:
            raise ValueError(f"{path}: 期待 {n*h*w} 字节，读到 {len(all_bytes)}")
    images = [all_bytes[i * h * w : (i + 1) * h * w] for i in range(n)]
    return n, h, w, images


def resize_area(src: bytes, src_w: int, src_h: int, target: int):
    """整数区域平均下采样。"""
    out = [0.0] * (target * target)
    for ti in range(target):
        sy0 = ti * src_h // target
        sy1 = (ti + 1) * src_h // target
        if sy1 == sy0:
            sy1 = sy0 + 1
        for tj in range(target):
            sx0 = tj * src_w // target
            sx1 = (tj + 1) * src_w // target
            if sx1 == sx0:
                sx1 = sx0 + 1
            total = 0
            count = 0
            for sy in range(sy0, sy1):
                row_start = sy * src_w
                for sx in range(sx0, sx1):
                    total += src[row_start + sx]
                    count += 1
            out[ti * target + tj] = total / count
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--src", required=True,
                    help="IDX3 文件路径 (支持 .gz)，如 data/mnist/train-images-idx3-ubyte.gz")
    ap.add_argument("--target", type=int, default=16,
                    help="重采样后的边长 (默认 16 → d=256)；指定 0 表示保持原尺寸")
    ap.add_argument("--limit", type=int, default=0,
                    help="只取前 N 张图 (默认 0 = 全部)")
    ap.add_argument("--out", required=True,
                    help="输出二进制路径，如 data/mnist_16x16_train200.bin")
    args = ap.parse_args()

    src = Path(args.src)
    n_total, h, w, images = read_idx_images(src)
    n_use = args.limit if (args.limit > 0 and args.limit < n_total) else n_total
    keep_original = (args.target <= 0)
    target = (h if keep_original else args.target)
    d = (h * w if keep_original else args.target * args.target)
    out_path = Path(args.out)
    meta_path = out_path.with_suffix(".meta.txt")

    print(f"IDX: {src}  原始 N={n_total}  h={h}  w={w}")
    print(f"使用 N={n_use}{('  → resize 到 ' + str(target) + 'x' + str(target)) if not keep_original else '  (保持原尺寸)'}")
    print(f"输出 d={d}  →  {out_path}")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(struct.pack("<II", n_use, d))
        for i in range(n_use):
            raw = images[i]
            if keep_original:
                vals = list(raw)
            else:
                vals = resize_area(raw, w, h, args.target)
            for v in vals:
                f.write(struct.pack("<d", float(v)))
            if (i + 1) % 100 == 0 or i == n_use - 1:
                print(f"  [{i+1:4d}/{n_use}]")

    with open(meta_path, "w") as f:
        f.write(f"source: {src}\n")
        f.write(f"N: {n_use}  (of total {n_total})\n")
        f.write(f"src_image_size: {h}x{w}\n")
        f.write(f"target_image_size: {target}x{target}\n" if not keep_original
                else f"target_image_size: kept {h}x{w}\n")
        f.write(f"d: {d}\n")
        f.write("resize: integer area-averaging downsample\n")
        f.write("pixel_range: 0..255 (未归一化，C++ 端负责中心化)\n")
        f.write("format: little-endian; [uint32 N][uint32 d][float64 row-major]\n")

    print(f"完成: {out_path} ({out_path.stat().st_size} 字节)")
    print(f"元数据: {meta_path}")


if __name__ == "__main__":
    sys.exit(main() or 0)
