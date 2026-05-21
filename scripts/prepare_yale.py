#!/usr/bin/env python3
"""Yale Face Database (165 张 PGM) → 16×16 灰度向量化二进制文件。

零依赖（仅 stdlib），供 C++ HE-PCA 加载。

输出格式（little-endian）：
  uint32 N         # 样本数（默认 165）
  uint32 d         # 特征维度 (= TARGET*TARGET)
  float64 × N × d  # row-major X[N, d]，0..255 未归一化
"""
import argparse
import struct
import sys
from pathlib import Path


def read_pgm_p5(path: Path):
    """读取 P5 (raw) PGM。返回 (width, height, bytes)。"""
    with open(path, "rb") as f:
        magic = f.readline().strip()
        if magic != b"P5":
            raise ValueError(f"{path} 不是 P5 PGM (got {magic!r})")

        def next_token():
            buf = b""
            # 跳过空白与注释，直到拿到一个 token
            while True:
                c = f.read(1)
                if not c:
                    raise ValueError("PGM header 提前结束")
                if c == b"#":
                    f.readline()
                    continue
                if c.isspace():
                    if buf:
                        return buf
                    continue
                buf += c
                while True:
                    c = f.read(1)
                    if not c or c.isspace():
                        return buf
                    buf += c

        w = int(next_token())
        h = int(next_token())
        maxval = int(next_token())
        if maxval > 255:
            raise NotImplementedError(f"maxval={maxval} > 255 暂不支持")
        data = f.read(w * h)
        if len(data) != w * h:
            raise ValueError(f"{path}: 读到 {len(data)} 字节，期待 {w*h}")
        return w, h, data


def resize_area(src: bytes, src_w: int, src_h: int, target: int):
    """整数区域平均下采样：每个目标像素 = 对应源矩形区域的均值。"""
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
    ap.add_argument("--src-dir", default="data/YALE/centered",
                    help="Yale PGM 图所在目录")
    ap.add_argument("--target", type=int, default=16,
                    help="重采样后的边长 (默认 16)")
    ap.add_argument("--limit", type=int, default=0,
                    help="只取前 N 张图 (默认 0 = 全部)")
    ap.add_argument("--out", default=None,
                    help="输出二进制路径 (默认 data/yale_<T>x<T>.bin)")
    args = ap.parse_args()

    src_dir = Path(args.src_dir)
    files = sorted(src_dir.glob("subject*.pgm"))
    if not files:
        sys.exit(f"找不到 PGM 文件: {src_dir}")
    if args.limit > 0:
        files = files[: args.limit]

    N = len(files)
    d = args.target * args.target
    out_path = Path(args.out) if args.out else Path(
        f"data/yale_{args.target}x{args.target}.bin")
    meta_path = out_path.with_suffix(".meta.txt")
    print(f"Yale: {N} 张图 → 输出维度 {d}（{args.target}x{args.target}）")
    print(f"输出: {out_path}")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(struct.pack("<II", N, d))
        for i, path in enumerate(files):
            w, h, raw = read_pgm_p5(path)
            small = resize_area(raw, w, h, args.target)
            for v in small:
                f.write(struct.pack("<d", v))
            if (i + 1) % 30 == 0 or i == N - 1:
                print(f"  [{i+1:3d}/{N}] {path.name}  src={w}x{h}")

    with open(meta_path, "w") as f:
        f.write("dataset: Yale Face Database (centered, P5 PGM)\n")
        f.write(f"source: {src_dir}\n")
        f.write(f"N: {N}\n")
        f.write(f"d: {d}\n")
        f.write(f"target_image_size: {args.target}x{args.target}\n")
        f.write("resize: integer area-averaging downsample\n")
        f.write("pixel_range: 0..255 (未归一化，C++ 端负责中心化)\n")
        f.write("format: little-endian; [uint32 N][uint32 d][float64 row-major]\n")

    print(f"完成: {out_path} ({out_path.stat().st_size} 字节)")
    print(f"元数据: {meta_path}")


if __name__ == "__main__":
    main()
