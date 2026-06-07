from pathlib import Path
from xml.sax.saxutils import escape


OUT = Path("/Users/bytedance/PCA/workspace/nudge-ppt/nudge_academic_protocol.svg")
W, H = 1920, 1080


def lines(text):
    return str(text).split("\n")


def text_block(x, y, text, size=20, color="#1f1f1f", weight="400", anchor="middle", line_h=None):
    if line_h is None:
        line_h = int(size * 1.28)
    out = [f'<text x="{x}" y="{y}" text-anchor="{anchor}" font-family="Arial, PingFang SC, Helvetica, sans-serif" font-size="{size}" font-weight="{weight}" fill="{color}">']
    for i, line in enumerate(lines(text)):
        dy = 0 if i == 0 else line_h
        out.append(f'<tspan x="{x}" dy="{dy}">{escape(line)}</tspan>')
    out.append("</text>")
    return "\n".join(out)


def box(x, y, w, h, title, body="", border="#002060", title_color=None, fill="#ffffff",
        sw=2, radius=14, title_size=19, body_size=16, dashed=False, body_color="#333333"):
    title_color = title_color or border
    dash = ' stroke-dasharray="10 7"' if dashed else ""
    cx = x + w / 2
    parts = [
        f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{radius}" ry="{radius}" fill="{fill}" stroke="{border}" stroke-width="{sw}"{dash}/>',
        text_block(cx, y + 28, title, title_size, title_color, "700"),
    ]
    if body:
        parts.append(text_block(cx, y + 56, body, body_size, body_color, "400"))
    return "\n".join(parts)


def label(x, y, w, h, text, fill, stroke, color="#1f1f1f", size=16, weight="700", radius=12):
    return "\n".join([
        f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{radius}" ry="{radius}" fill="{fill}" stroke="{stroke}" stroke-width="2"/>',
        text_block(x + w / 2, y + h / 2 + size / 3, text, size, color, weight)
    ])


def arrow(x1, y1, x2, y2, color="#7F7F7F", sw=2, dashed=False, marker="arrow"):
    dash = ' stroke-dasharray="9 7"' if dashed else ""
    marker_end = f' marker-end="url(#{marker})"' if marker else ""
    return f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" stroke-width="{sw}"{dash}{marker_end} fill="none"/>'


def poly(points, color="#7F7F7F", sw=2, dashed=False, marker="arrow"):
    pts = " ".join(f"{x},{y}" for x, y in points)
    dash = ' stroke-dasharray="9 7"' if dashed else ""
    marker_end = f' marker-end="url(#{marker})"' if marker else ""
    return f'<polyline points="{pts}" stroke="{color}" stroke-width="{sw}"{dash}{marker_end} fill="none"/>'


def user_icon(cx, cy, scale=1.0, color="#548235"):
    r = 12 * scale
    return "\n".join([
        f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="#ffffff" stroke="{color}" stroke-width="2"/>',
        f'<path d="M {cx-22*scale} {cy+35*scale} Q {cx} {cy+10*scale} {cx+22*scale} {cy+35*scale}" fill="none" stroke="{color}" stroke-width="3" stroke-linecap="round"/>'
    ])


def server_icon(x, y, label_txt, color="#002060"):
    return "\n".join([
        f'<rect x="{x}" y="{y}" width="42" height="58" rx="5" fill="#ffffff" stroke="{color}" stroke-width="2"/>',
        f'<line x1="{x+9}" y1="{y+15}" x2="{x+33}" y2="{y+15}" stroke="{color}" stroke-width="2"/>',
        f'<line x1="{x+9}" y1="{y+30}" x2="{x+33}" y2="{y+30}" stroke="{color}" stroke-width="2"/>',
        f'<circle cx="{x+14}" cy="{y+45}" r="3" fill="{color}"/>',
        text_block(x + 21, y + 80, label_txt, 14, color, "700")
    ])


svg = []
svg.append(f'''<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">
<defs>
  <marker id="arrow" markerWidth="10" markerHeight="10" refX="8" refY="3" orient="auto" markerUnits="strokeWidth">
    <path d="M0,0 L0,6 L8,3 z" fill="#7F7F7F"/>
  </marker>
  <marker id="arrowBlue" markerWidth="10" markerHeight="10" refX="8" refY="3" orient="auto" markerUnits="strokeWidth">
    <path d="M0,0 L0,6 L8,3 z" fill="#002060"/>
  </marker>
  <marker id="arrowRed" markerWidth="10" markerHeight="10" refX="8" refY="3" orient="auto" markerUnits="strokeWidth">
    <path d="M0,0 L0,6 L8,3 z" fill="#C00000"/>
  </marker>
  <style>
    .small {{ font-family: Arial, PingFang SC, Helvetica, sans-serif; }}
  </style>
</defs>
<rect x="0" y="0" width="{W}" height="{H}" fill="#FFFFFF"/>
''')

# Header
svg.append(text_block(760, 48, "Nudge: A Private Recommendations Engine", 27, "#002060", "700"))
svg.append(text_block(960, 79, "Three-Party Secure Matrix Factorization via Power Iteration · Henzinger et al.", 14, "#7F7F7F", "400"))

# Legend
legend_x, legend_y = 1280, 18
svg.append(f'<rect x="{legend_x}" y="{legend_y}" width="595" height="92" rx="14" fill="#ffffff" stroke="#D9D9D9" stroke-width="1.5"/>')
svg.append(text_block(legend_x + 22, legend_y + 25, "图例", 14, "#002060", "700", anchor="start"))
legend_items = [
    ("Users", "#548235", "用户"),
    ("Servers", "#002060", "三台服务器"),
    ("3PC", "#C00000", "三方计算核心"),
    ("Primitives", "#7F7F7F", "密码学原语"),
    ("Recommendations", "#ED7D31", "推荐输出"),
]
for i, (name, color, desc) in enumerate(legend_items):
    x = legend_x + 18 + i * 112
    svg.append(f'<rect x="{x}" y="{legend_y+38}" width="18" height="18" rx="4" fill="#ffffff" stroke="{color}" stroke-width="3"/>')
    svg.append(text_block(x + 26, legend_y + 52, desc, 12, "#333333", "400", anchor="start"))
svg.append(text_block(legend_x + 20, legend_y + 80, "实线=数据/秘密份额流   虚线=迭代/调用   ⟦·⟧=秘密共享态   B=明文/公开模型", 11, "#666666", "400", anchor="start"))

# Stage bands
stage_specs = [
    (125, 215, "阶段 ①  Private Data Collection  私有数据收集"),
    (360, 470, "阶段 ②  Private Matrix Factorization  私有矩阵分解"),
    (850, 120, "阶段 ③  Private Recommendation Serving  隐私推荐分发"),
]
for y, h, t in stage_specs:
    svg.append(f'<rect x="28" y="{y}" width="1864" height="{h}" rx="18" fill="#FAFAFA" stroke="#E3E3E3" stroke-width="1.5"/>')
    svg.append(text_block(52, y + 27, t, 18, "#002060" if y != 360 else "#C00000", "700", anchor="start"))

# Three server lanes, spanning all stages
lane_x = [405, 720, 1035]
lane_w = 285
for idx, x in enumerate(lane_x, 1):
    svg.append(f'<rect x="{x}" y="155" width="{lane_w}" height="805" rx="18" fill="#F3F6FA" stroke="#BFC7D5" stroke-width="1.5"/>')
    svg.append(server_icon(x + 18, 167, f"Server {idx}", "#002060"))
    svg.append(text_block(x + lane_w / 2 + 25, 183, f"Server {idx} 泳道", 17, "#002060", "700"))
    svg.append(text_block(x + lane_w / 2 + 25, 205, "non-colluding party", 12, "#7F7F7F", "400"))

# Users
user_positions = [(130, 185), (130, 235), (130, 285), (260, 210), (260, 265)]
for i, (cx, cy) in enumerate(user_positions, 1):
    svg.append(user_icon(cx, cy, 0.82, "#548235"))
    svg.append(label(cx - 47, cy + 38, 94, 38, f"u^({i}) ∈ R^n\n评分向量", "#FFFFFF", "#548235", "#548235", 10, "700", 8))

# Secret sharing lines from users to lanes
share_colors = ["#002060", "#5B9BD5", "#9DC3E6"]
server_hold_y = 240
for i, x in enumerate(lane_x):
    svg.append(box(x + 32, server_hold_y, 220, 70, f"Server {i+1} 持有", f"⟦U⟧_{i+1}\nU ∈ R^(m×n)", "#002060", "#002060", sw=2, title_size=15, body_size=14))
svg.append(f'<rect x="250" y="216" width="120" height="82" rx="10" ry="10" fill="#FFFFFF" stroke="#548235" stroke-width="2"/>')
svg.append(text_block(310, 250, "share bundle", 12, "#548235", "700"))
svg.append(arrow(220, 240, 240, 240, "#548235", 2, marker="arrow"))
for j, x in enumerate(lane_x):
    y = 235 + j * 18
    svg.append(poly([(390, y), (x + 32, y)], share_colors[j], 2.0, marker="arrowBlue"))
svg.append(label(408, 324, 215, 28, "Replicated Secret Sharing", "#FFFFFF", "#7F7F7F", "#333333", 12, "700", 8))
svg.append(box(1405, 230, 400, 82, "(可选) Input Validation", "轻量一次性合法性校验\n几乎零开销", "#7F7F7F", "#7F7F7F", sw=2, title_size=16, body_size=14))

# 3PC core box
core_x, core_y, core_w, core_h = 390, 390, 960, 410
svg.append(f'<rect x="{core_x}" y="{core_y}" width="{core_w}" height="{core_h}" rx="22" fill="#FFFFFF" stroke="#C00000" stroke-width="3"/>')
svg.append(text_block(core_x + core_w / 2, core_y + 26, "Three-Party Power Iteration\n算法 ApproxFactor", 17, "#C00000", "700"))

# Security tag
svg.append(label(core_x + core_w - 345, core_y + 48, 325, 52, "安全模型: honest-but-curious · honest majority\n限制: 不防 malicious / 2 台合谋 / metadata", "#FFF7F7", "#C00000", "#333333", 12, "700", 10))

# Substeps
svg.append(box(core_x + 35, core_y + 72, 270, 105, "2.1 初始化", "for i = 1 → d\n⟦v⟧ ∈ R^n\nv := Normalize(SetOrthogonal(v,B))\nB ∈ R^(d×n) 初始为 0", "#C00000", "#C00000", sw=2, title_size=17, body_size=13))

loop_x, loop_y, loop_w, loop_h = core_x + 330, core_y + 115, 585, 185
svg.append(f'<rect x="{loop_x}" y="{loop_y}" width="{loop_w}" height="{loop_h}" rx="18" fill="#FFFDFD" stroke="#C00000" stroke-width="2.5" stroke-dasharray="10 7"/>')
svg.append(text_block(loop_x + 20, loop_y + 28, "2.2 幂迭代主循环", 17, "#C00000", "700", anchor="start"))
svg.append(label(loop_x + loop_w - 205, loop_y + 13, 185, 30, "ℓ 轮迭代, O(log(n/ε)/ω)", "#FFFFFF", "#C00000", "#C00000", 12, "700", 10))
loop_steps = [
    ("a", "⟦t⟧ := Mul(U, ⟦v⟧)", "step a: 线性乘法, 非交互"),
    ("b", "⟦v⟧ := Mul(U^T, ⟦t⟧)", "矩阵-向量乘 ×2"),
    ("c", "⟦v⟧ := SetOrthogonal(⟦v⟧, B)", "Gram-Schmidt 正交化"),
    ("d", "⟦v⟧ := Normalize(⟦v⟧)", "交互非线性函数, L2 归一化"),
]
for k, (tag, main, note) in enumerate(loop_steps):
    x = loop_x + 30 + (k % 2) * 275
    y = loop_y + 58 + (k // 2) * 62
    svg.append(box(x, y, 250, 48, f"{tag}. {main}", note, "#C00000", "#C00000", sw=1.8, radius=10, title_size=12, body_size=10))
svg.append(poly([(loop_x + 300, loop_y + 168), (loop_x + 300, loop_y + 205), (loop_x + 35, loop_y + 205), (loop_x + 35, loop_y + 78)], "#C00000", 2, dashed=True, marker="arrowRed"))

svg.append(box(core_x + 35, core_y + 210, 270, 92, "2.3 收敛输出", "B_i := v\n循环至 i = d\n明文 B 由三方共同持有", "#C00000", "#C00000", sw=2, title_size=17, body_size=13))
svg.append(box(core_x + 35, core_y + 325, 410, 66, "2.4 计算用户嵌入", "⟦A⟧ := ⟦U⟧ · B^T,  A ∈ R^(m×d) 仍为秘密共享", "#C00000", "#C00000", sw=2, title_size=17, body_size=13))
svg.append(label(core_x + 475, core_y + 340, 425, 36, "Goal — argmin ||U − A·B||_F；等价于求 U^T U 的 top-d 特征向量", "#FFFFFF", "#C00000", "#C00000", 13, "700", 10))

# Primitive toolbox
tool_x, tool_y, tool_w, tool_h = 1395, 390, 445, 410
svg.append(f'<rect x="{tool_x}" y="{tool_y}" width="{tool_w}" height="{tool_h}" rx="18" fill="#F7F7F7" stroke="#7F7F7F" stroke-width="2"/>')
svg.append(text_block(tool_x + tool_w / 2, tool_y + 33, "Cryptographic Primitives", 20, "#7F7F7F", "700"))
prims = [
    ("Replicated Secret Sharing", "Araki et al."),
    ("Function Secret Sharing", "Boyle et al."),
    ("Truncate-c bits", "3 轮 3PC, 通信 2λc(λ+4)+10λβ\n比 SOTA 节省 6×"),
    ("Approx. Normalize", "1 轮启动 + Newton-Raphson\n无额外泄漏"),
]
for i, (t, b) in enumerate(prims):
    svg.append(box(tool_x + 30, tool_y + 58 + i * 83, 385, 68, t, b, "#7F7F7F", "#7F7F7F", sw=1.8, title_size=15, body_size=11))
svg.append(text_block(tool_x + 28, tool_y + tool_h - 20, "支撑阶段②的线性与非线性安全计算", 12, "#7F7F7F", "700", anchor="start"))

# Stage transitions
svg.append(arrow(890, 340, 890, 390, "#C00000", 5, marker="arrowRed"))
svg.append(arrow(890, 800, 890, 850, "#ED7D31", 5, marker="arrow"))

# Serving stage
serve_x, serve_y, serve_w, serve_h = 1035, 874, 510, 74
svg.append(box(serve_x, serve_y, serve_w, serve_h, "Serve recommendations to User i", "三台服务器计算 ⟦scores⟧ = ⟦a^(i)⟧ · B；份额回传；User i 本地重构 Top-k", "#ED7D31", "#ED7D31", sw=2.5, title_size=17, body_size=13))
svg.append(user_icon(1635, 910, 1.0, "#548235"))
svg.append(label(1582, 950, 106, 38, "User i\nTop-k 推荐", "#FFFFFF", "#548235", "#548235", 11, "700", 8))
svg.append(arrow(core_x + core_w / 2, core_y + core_h, serve_x + 80, serve_y, "#ED7D31", 4, marker="arrow"))
for j, color in enumerate(share_colors):
    y = serve_y + 32 + j * 9
    svg.append(arrow(serve_x + serve_w, y, 1608, y, color, 1.6, marker="arrowBlue"))

# Bottom performance strip
strip_y = 970
svg.append(f'<rect x="28" y="{strip_y}" width="1864" height="84" rx="14" fill="#F2F2F2" stroke="#D9D9D9" stroke-width="1.5"/>')
cards = [
    ("Netflix", "500K users × 10K items\n50 min, 40 GB, nDCG@20=0.29"),
    ("Criteo", "millions users × hundreds campaigns\n1.5 h, 124 GB"),
    ("Yelp", "100K+ × 100K+\n8 h, 250 GB"),
    ("Speedup", "比 2PC 乱码电路快 4 个数量级\n比 4PC 通信少 8×"),
]
for i, (t, b) in enumerate(cards):
    x = 54 + i * 455
    svg.append(box(x, strip_y + 8, 415, 68, t, b, "#7F7F7F", "#002060", fill="#FFFFFF", sw=1.3, radius=10, title_size=12, body_size=10))

svg.append(text_block(960, 1070, "Source: Henzinger, Dauterman, Corrigan-Gibbs, Boneh. Nudge: A Private Recommendations Engine.", 10, "#7F7F7F", "400"))
svg.append("</svg>")

OUT.write_text("\n".join(svg), encoding="utf-8")
print(OUT)
