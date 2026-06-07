from pathlib import Path
from xml.sax.saxutils import escape


OUT = Path("/Users/bytedance/PCA/workspace/nudge-ppt/nudge_minimal_protocol.svg")
W, H = 1920, 1080


def text_block(x, y, text, size=18, color="#1f1f1f", weight="400", anchor="middle", line_h=None):
    if line_h is None:
        line_h = int(size * 1.25)
    parts = [f'<text x="{x}" y="{y}" text-anchor="{anchor}" font-family="Arial, PingFang SC, Helvetica, sans-serif" font-size="{size}" font-weight="{weight}" fill="{color}">']
    for i, line in enumerate(str(text).split("\n")):
        dy = 0 if i == 0 else line_h
        parts.append(f'<tspan x="{x}" dy="{dy}">{escape(line)}</tspan>')
    parts.append("</text>")
    return "\n".join(parts)


def box(x, y, w, h, title, body="", border="#002060", fill="#FFFFFF", title_color=None,
        body_color="#333333", sw=2, radius=14, title_size=18, body_size=14, dashed=False):
    title_color = title_color or border
    dash = ' stroke-dasharray="10 7"' if dashed else ""
    cx = x + w / 2
    out = [
        f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{radius}" ry="{radius}" fill="{fill}" stroke="{border}" stroke-width="{sw}"{dash}/>',
        text_block(cx, y + 28, title, title_size, title_color, "700"),
    ]
    if body:
        out.append(text_block(cx, y + 58, body, body_size, body_color, "400"))
    return "\n".join(out)


def arrow(x1, y1, x2, y2, color="#7F7F7F", sw=2.5, dashed=False):
    dash = ' stroke-dasharray="9 7"' if dashed else ""
    return f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" stroke-width="{sw}"{dash} marker-end="url(#arrow)" fill="none"/>'


def poly(points, color="#7F7F7F", sw=2.5, dashed=False):
    dash = ' stroke-dasharray="9 7"' if dashed else ""
    pts = " ".join(f"{x},{y}" for x, y in points)
    return f'<polyline points="{pts}" stroke="{color}" stroke-width="{sw}"{dash} marker-end="url(#arrow)" fill="none"/>'


def pill(x, y, w, h, text, color, fill="#FFFFFF", size=13):
    return "\n".join([
        f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{h/2}" ry="{h/2}" fill="{fill}" stroke="{color}" stroke-width="2"/>',
        text_block(x + w / 2, y + h / 2 + size / 3, text, size, color, "700")
    ])


svg = [f'''<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">
<defs>
  <marker id="arrow" markerWidth="10" markerHeight="10" refX="8" refY="3" orient="auto" markerUnits="strokeWidth">
    <path d="M0,0 L0,6 L8,3 z" fill="#7F7F7F"/>
  </marker>
</defs>
<rect x="0" y="0" width="{W}" height="{H}" fill="#FFFFFF"/>
''']

# Header
svg.append(text_block(960, 48, "Nudge: A Private Recommendations Engine", 28, "#002060", "700"))
svg.append(text_block(960, 78, "Three-party secure matrix factorization via power iteration · Henzinger et al.", 14, "#7F7F7F", "400"))

# Compact legend
legend_y = 94
legend_items = [
    ("Users", "#548235"),
    ("Servers", "#002060"),
    ("3PC ApproxFactor", "#C00000"),
    ("Primitives", "#7F7F7F"),
    ("Recommendations", "#ED7D31"),
]
start_x = 420
for i, (name, color) in enumerate(legend_items):
    svg.append(pill(start_x + i * 220, legend_y, 185, 30, name, color, "#FFFFFF", 12))

# Stage backgrounds
stages = [
    (50, 165, 1820, 170, "1. Private Data Collection  私有数据收集", "#002060"),
    (50, 360, 1820, 390, "2. Private Matrix Factorization  私有矩阵分解", "#C00000"),
    (50, 775, 1820, 150, "3. Private Recommendation Serving  隐私推荐分发", "#ED7D31"),
]
for x, y, w, h, title, color in stages:
    svg.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="18" fill="#FAFAFA" stroke="#E0E0E0" stroke-width="1.5"/>')
    svg.append(text_block(x + 24, y + 30, title, 18, color, "700", anchor="start"))

# Stage 1: Collection
svg.append(box(115, 215, 250, 85, "Users", "每个用户本地持有评分向量\nu^(i) ∈ R^n；原始评分不外流", "#548235", title_size=18, body_size=13))
svg.append(box(455, 215, 250, 85, "Secret-share input", "Replicated Secret Sharing\nu^(i) -> ⟦u^(i)⟧_1, ⟦u^(i)⟧_2, ⟦u^(i)⟧_3", "#7F7F7F", title_size=18, body_size=12))
svg.append(f'<rect x="820" y="200" width="520" height="112" rx="16" fill="#F4F7FB" stroke="#002060" stroke-width="2"/>')
svg.append(text_block(1080, 228, "Three non-colluding servers", 18, "#002060", "700"))
for i, sx in enumerate([855, 1015, 1175], 1):
    svg.append(box(sx, 245, 130, 50, f"Server {i}", f"holds ⟦U⟧_{i}", "#002060", title_size=13, body_size=11, radius=8))
svg.append(box(1465, 218, 300, 78, "Input Validation", "可选：一次性合法性校验\n轻量开销", "#7F7F7F", title_size=16, body_size=12))
svg.append(arrow(365, 257, 455, 257, "#548235", 3))
for yy in [244, 257, 270]:
    svg.append(arrow(705, yy, 820, yy, "#002060", 2.2))

# Stage 2: Core algorithm
core_x, core_y, core_w, core_h = 215, 405, 1080, 300
svg.append(f'<rect x="{core_x}" y="{core_y}" width="{core_w}" height="{core_h}" rx="18" fill="#FFFFFF" stroke="#C00000" stroke-width="3"/>')
svg.append(text_block(core_x + core_w / 2, core_y + 32, "Three-Party Power Iteration / ApproxFactor", 22, "#C00000", "700"))
svg.append(text_block(core_x + core_w / 2, core_y + 58, "核心只保留算法主链：初始化 -> 幂迭代 -> 输出 B 与 ⟦A⟧", 13, "#666666", "400"))

svg.append(box(core_x + 35, core_y + 88, 230, 88, "2.1 初始化", "for i=1..d\n随机 ⟦v⟧；正交化并归一化\nB 初始为 0", "#C00000", title_size=16, body_size=12))
loop_x, loop_y, loop_w, loop_h = core_x + 310, core_y + 83, 430, 150
svg.append(f'<rect x="{loop_x}" y="{loop_y}" width="{loop_w}" height="{loop_h}" rx="14" fill="#FFFDFD" stroke="#C00000" stroke-width="2.5" stroke-dasharray="10 7"/>')
svg.append(text_block(loop_x + loop_w / 2, loop_y + 30, "2.2 幂迭代主循环（ℓ rounds）", 17, "#C00000", "700"))
svg.append(text_block(loop_x + loop_w / 2, loop_y + 66, "⟦v⟧ <- U^T (U ⟦v⟧)\n⟦v⟧ <- SetOrthogonal(⟦v⟧, B)\n⟦v⟧ <- Normalize(⟦v⟧)", 15, "#333333", "400"))
svg.append(text_block(loop_x + loop_w / 2, loop_y + 132, "linear mul: non-interactive；nonlinear: 3PC/FSS", 12, "#7F7F7F", "400"))
svg.append(box(core_x + 785, core_y + 88, 230, 88, "2.3 输出模型", "B_i := 收敛后的 v\nB ∈ R^(d×n)\n公开模型", "#C00000", title_size=16, body_size=12))
svg.append(box(core_x + 785, core_y + 195, 230, 70, "2.4 用户嵌入", "⟦A⟧ := ⟦U⟧ · B^T\n仍为秘密共享", "#C00000", title_size=16, body_size=12))
svg.append(arrow(core_x + 265, core_y + 132, loop_x, core_y + 132, "#C00000", 3, False))
svg.append(arrow(loop_x + loop_w, core_y + 132, core_x + 785, core_y + 132, "#C00000", 3, False))
svg.append(arrow(core_x + 900, core_y + 176, core_x + 900, core_y + 195, "#C00000", 2.5, False))
svg.append(poly([(loop_x + 215, loop_y + 140), (loop_x + 215, loop_y + 166), (loop_x + 35, loop_y + 166), (loop_x + 35, loop_y + 50)], "#C00000", 2, True))
svg.append(text_block(core_x + 35, core_y + core_h - 24, "Goal: argmin ||U - A·B||_F；等价于求 U^T U 的 top-d 特征向量", 14, "#C00000", "700", anchor="start"))

# Side notes merged
svg.append(box(1360, 415, 440, 135, "Cryptographic primitives", "Replicated Secret Sharing；Function Secret Sharing\nTruncation；Approx. Normalize\n用于支撑阶段 2 的线性乘法与非线性函数", "#7F7F7F", fill="#FFFFFF", title_size=18, body_size=13))
svg.append(box(1360, 580, 440, 100, "Security model", "honest-but-curious；3-party honest majority\n保护 1 台服务器被 compromise 时的用户隐私\n限制：不防 malicious / 2 台合谋 / metadata 泄漏", "#002060", fill="#FFFFFF", title_size=18, body_size=12))
svg.append(arrow(1080, 312, 1080, 405, "#C00000", 5))

# Stage 3: Serving
svg.append(box(270, 825, 320, 72, "Server-side scoring", "三方计算 ⟦scores⟧ = ⟦a^(i)⟧ · B", "#002060", title_size=17, body_size=13))
svg.append(box(800, 825, 320, 72, "Return shares", "服务器分别发送 scores 的秘密份额", "#ED7D31", title_size=17, body_size=13))
svg.append(box(1330, 825, 320, 72, "User-side reconstruction", "User i 本地重构并选择 Top-k item", "#548235", title_size=17, body_size=13))
svg.append(arrow(590, 861, 800, 861, "#ED7D31", 4))
svg.append(arrow(1120, 861, 1330, 861, "#ED7D31", 4))

# Footer
svg.append(f'<rect x="50" y="955" width="1820" height="78" rx="14" fill="#F2F2F2" stroke="#D9D9D9" stroke-width="1.5"/>')
footer = (
    "实验结论：Netflix 50 min / 40 GB / nDCG@20=0.29；"
    "Criteo 1.5 h / 124 GB；Yelp 8 h / 250 GB；"
    "相较 2PC 乱码电路快 4 个数量级，相较 4PC 通信少 8×"
)
svg.append(text_block(960, 987, footer, 15, "#002060", "700"))
svg.append(text_block(960, 1020, "Source: Henzinger, Dauterman, Corrigan-Gibbs, Boneh. Nudge: A Private Recommendations Engine.", 11, "#7F7F7F", "400"))

svg.append("</svg>")
OUT.write_text("\n".join(svg), encoding="utf-8")
print(OUT)
