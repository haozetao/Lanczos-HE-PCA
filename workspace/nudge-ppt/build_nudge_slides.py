import json
import math
from pathlib import Path

import fitz
from PIL import Image, ImageDraw, ImageFont, ImageFilter


ROOT = Path("/Users/bytedance/PCA")
WORK = ROOT / "workspace" / "nudge-ppt"
OUT = WORK / "outputs"
PDF = ROOT / "paper" / "Henzinger 等 - Nudge A Private Recommendations Engine.pdf"

W, H = 1920, 1080

BG = (247, 244, 238)
INK = (29, 34, 43)
MUTED = (93, 101, 116)
LINE = (222, 214, 202)
ACCENT = (42, 99, 145)
ACCENT2 = (176, 82, 63)
GREEN = (76, 126, 90)
YELLOW = (222, 171, 69)
DARK = (22, 27, 35)
WHITE = (255, 255, 255)


def font(size, weight="regular"):
    candidates = []
    if weight == "bold":
        candidates = [
            "/System/Library/Fonts/STHeiti Medium.ttc",
            "/System/Library/Fonts/Hiragino Sans GB.ttc",
            "/System/Library/Fonts/HelveticaNeue.ttc",
            "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
            "/Library/Fonts/Arial Bold.ttf",
        ]
    else:
        candidates = [
            "/System/Library/Fonts/STHeiti Light.ttc",
            "/System/Library/Fonts/Hiragino Sans GB.ttc",
            "/System/Library/Fonts/HelveticaNeue.ttc",
            "/System/Library/Fonts/Supplemental/Arial.ttf",
            "/Library/Fonts/Arial.ttf",
        ]
    for path in candidates:
        try:
            return ImageFont.truetype(path, size=size)
        except Exception:
            pass
    return ImageFont.load_default()


F_TITLE = font(62, "bold")
F_TITLE2 = font(50, "bold")
F_H2 = font(34, "bold")
F_H3 = font(28, "bold")
F_BODY = font(27)
F_BODY_B = font(27, "bold")
F_SMALL = font(21)
F_SMALL_B = font(21, "bold")
F_TINY = font(17)


def text_size(draw, text, f):
    box = draw.textbbox((0, 0), text, font=f)
    return box[2] - box[0], box[3] - box[1]


def wrap_text(draw, text, f, max_width):
    lines = []
    for para in str(text).split("\n"):
        if not para:
            lines.append("")
            continue
        buf = ""
        for ch in para:
            test = buf + ch
            if text_size(draw, test, f)[0] <= max_width:
                buf = test
            else:
                if buf:
                    lines.append(buf)
                buf = ch
        if buf:
            lines.append(buf)
    return lines


def draw_wrapped(draw, xy, text, f, fill=INK, max_width=600, line_gap=9, max_lines=None):
    x, y = xy
    lines = wrap_text(draw, text, f, max_width)
    if max_lines:
        lines = lines[:max_lines]
    for line in lines:
        draw.text((x, y), line, font=f, fill=fill)
        y += f.size + line_gap
    return y


def rounded(draw, box, fill, outline=None, width=2, radius=28):
    draw.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)


def arrow(draw, start, end, fill=ACCENT, width=5):
    draw.line([start, end], fill=fill, width=width)
    angle = math.atan2(end[1] - start[1], end[0] - start[0])
    length = 18
    for delta in (2.55, -2.55):
        p = (end[0] - length * math.cos(angle + delta), end[1] - length * math.sin(angle + delta))
        draw.line([end, p], fill=fill, width=width)


def base(title=None, kicker=None, dark=False):
    img = Image.new("RGB", (W, H), DARK if dark else BG)
    draw = ImageDraw.Draw(img)
    if dark:
        draw.rectangle((0, 0, W, 18), fill=ACCENT2)
        fill = WHITE
        sub = (196, 203, 213)
    else:
        draw.rectangle((0, 0, W, 18), fill=ACCENT)
        fill = INK
        sub = MUTED
    if kicker:
        draw.text((86, 62), kicker.upper(), font=F_TINY, fill=ACCENT2 if not dark else (234, 173, 146))
    if title:
        draw.text((86, 94), title, font=F_TITLE2, fill=fill)
    return img, draw, fill, sub


def paste_cover(dst, src, box):
    x1, y1, x2, y2 = box
    tw, th = x2 - x1, y2 - y1
    sw, sh = src.size
    scale = max(tw / sw, th / sh)
    resized = src.resize((int(sw * scale), int(sh * scale)), Image.Resampling.LANCZOS)
    left = (resized.width - tw) // 2
    top = (resized.height - th) // 2
    crop = resized.crop((left, top, left + tw, top + th))
    dst.paste(crop, (x1, y1))


def add_paper_crop(img, page_num, rel, box, border=True, caption=None):
    doc = fitz.open(str(PDF))
    page = doc[page_num - 1]
    rect = page.rect
    crop = fitz.Rect(
        rect.x0 + rel[0] * rect.width,
        rect.y0 + rel[1] * rect.height,
        rect.x0 + rel[2] * rect.width,
        rect.y0 + rel[3] * rect.height,
    )
    pix = page.get_pixmap(matrix=fitz.Matrix(3, 3), clip=crop, alpha=False)
    tmp = Image.frombytes("RGB", [pix.width, pix.height], pix.samples)
    x1, y1, x2, y2 = box
    tmp.thumbnail((x2 - x1, y2 - y1), Image.Resampling.LANCZOS)
    card = Image.new("RGB", (x2 - x1, y2 - y1), WHITE)
    card.paste(tmp, ((card.width - tmp.width) // 2, (card.height - tmp.height) // 2))
    if border:
        card = card.filter(ImageFilter.UnsharpMask(radius=1, percent=130, threshold=3))
    img.paste(card, (x1, y1))
    d = ImageDraw.Draw(img)
    if border:
        d.rounded_rectangle((x1, y1, x2, y2), radius=18, outline=LINE, width=3)
    if caption:
        d.text((x1, y2 + 14), caption, font=F_TINY, fill=MUTED)


def chip(draw, xy, text, fill=(235, 229, 218), fg=INK):
    x, y = xy
    tw, th = text_size(draw, text, F_SMALL_B)
    rounded(draw, (x, y, x + tw + 34, y + 42), fill, radius=21)
    draw.text((x + 17, y + 10), text, font=F_SMALL_B, fill=fg)
    return x + tw + 48


def bullet_list(draw, x, y, items, max_width, f=F_BODY, fill=INK, gap=18, dot=ACCENT):
    for item in items:
        draw.ellipse((x, y + 10, x + 10, y + 20), fill=dot)
        y = draw_wrapped(draw, (x + 26, y), item, f, fill, max_width=max_width, line_gap=7) + gap
    return y


def slide1():
    img, d, _, _ = base(dark=True)
    d.text((86, 82), "PAPER DECK", font=F_TINY, fill=(238, 177, 146))
    d.text((86, 146), "Nudge", font=font(104, "bold"), fill=WHITE)
    d.text((86, 262), "A Private Recommendations Engine", font=F_TITLE2, fill=(226, 231, 237))
    d.text((90, 372), "Henzinger · Dauterman · Corrigan-Gibbs · Boneh", font=F_BODY, fill=(187, 195, 207))
    d.text((90, 420), "Cryptographic privacy for large-scale recommender systems", font=F_BODY, fill=(187, 195, 207))
    rounded(d, (1050, 142, 1770, 790), (31, 39, 51), outline=(73, 84, 101), radius=34)
    for i, label in enumerate(["Users", "3 non-colluding servers", "Secret-shared ratings", "Private matrix factorization", "Personalized recommendations"]):
        y = 205 + i * 105
        rounded(d, (1110, y, 1700, y + 66), (44, 53, 68), radius=18)
        d.text((1140, y + 18), label, font=F_SMALL_B, fill=WHITE)
        if i < 4:
            arrow(d, (1405, y + 73), (1405, y + 100), fill=(214, 129, 91), width=4)
    d.text((90, 930), "目标：不暴露用户行为数据，同时训练可用的推荐模型", font=F_H3, fill=(238, 177, 146))
    return img


def slide2():
    img, d, _, _ = base("为什么需要 Nudge？", "Motivation")
    left = [
        ("推荐系统", "依赖点击、浏览、评分等高度敏感行为数据"),
        ("已有路径", "匿名代理 / 联邦学习 / TEE / 传统 MPC 均有规模或泄露问题"),
        ("Nudge 目标", "在强密码学隐私下，仍能处理百万级用户或物品"),
    ]
    y = 205
    for i, (k, v) in enumerate(left):
        rounded(d, (86, y, 880, y + 132), WHITE, outline=LINE, radius=28)
        d.text((126, y + 28), k, font=F_H3, fill=ACCENT if i != 2 else ACCENT2)
        draw_wrapped(d, (300, y + 30), v, F_BODY, INK, max_width=520)
        y += 165
    rounded(d, (1030, 210, 1760, 790), (31, 39, 51), radius=34)
    d.text((1090, 262), "核心承诺", font=F_H2, fill=WHITE)
    bullet_list(
        d,
        1095,
        340,
        [
            "服务器看不到单个用户评了什么、评分是多少",
            "用户不参与训练过程，只提交评分并接收推荐",
            "泄露仅限最终聚合模型 B 和每个用户提交评分数量",
            "容忍 1 台服务器被半诚实攻陷",
        ],
        560,
        f=F_BODY,
        fill=(228, 234, 240),
        dot=(214, 129, 91),
    )
    return img


def slide3():
    img, d, _, _ = base("系统总览：三服务器拆分信任", "Architecture")
    add_paper_crop(img, 4, (0.04, 0.08, 0.47, 0.34), (88, 208, 820, 555), caption="论文 Figure 1：Nudge 协议流")
    steps = [
        ("1", "Private data collection", "用户把评分向量 secret-share 后分别发给 3 台服务器"),
        ("2", "Private matrix factorization", "服务器在 3PC 中训练 item embeddings B 与 user shares A"),
        ("3", "Private recommendations", "服务器返回推荐分数的 share，用户本地重构结果"),
    ]
    y = 225
    for no, title, desc in steps:
        x = 930
        d.ellipse((x, y, x + 56, y + 56), fill=ACCENT)
        d.text((x + 19, y + 13), no, font=F_SMALL_B, fill=WHITE)
        d.text((x + 80, y - 2), title, font=F_H3, fill=INK)
        draw_wrapped(d, (x + 80, y + 40), desc, F_SMALL, MUTED, max_width=700)
        y += 150
    rounded(d, (930, 735, 1740, 870), (233, 226, 214), radius=22)
    d.text((970, 768), "和联邦学习的关键区别", font=F_H3, fill=ACCENT2)
    draw_wrapped(d, (970, 815), "用户不参与多轮训练；训练过程不向用户或服务器暴露中间模型版本。", F_SMALL, INK, max_width=700)
    return img


def slide4():
    img, d, _, _ = base("威胁模型：强隐私，但不是万能", "Security Model")
    cx, cy, r = 760, 500, 220
    for idx, (ang, label) in enumerate([(math.radians(270), "S0"), (math.radians(30), "S1"), (math.radians(150), "S2")]):
        x = cx + r * math.cos(ang)
        y = cy + r * math.sin(ang)
        d.ellipse((x - 64, y - 64, x + 64, y + 64), fill=(31, 39, 51))
        d.text((x - 22, y - 18), label, font=F_H2, fill=WHITE)
        arrow(d, (cx, cy), (x * 0.86 + cx * 0.14, y * 0.86 + cy * 0.14), fill=LINE, width=4)
    d.ellipse((cx - 92, cy - 92, cx + 92, cy + 92), fill=ACCENT)
    d.text((cx - 53, cy - 20), "2-out-of-3", font=F_SMALL_B, fill=WHITE)
    d.text((cx - 38, cy + 14), "sharing", font=F_SMALL_B, fill=WHITE)
    box_y = 210
    cards = [
        ("保护对象", "用户评分矩阵 U 的位置和值；其他用户也无法学习单个用户行为。", GREEN),
        ("允许泄露", "聚合模型 B 所包含的信息；每个用户提交了多少个非零评分。", YELLOW),
        ("不保护", "2 台及以上服务器串谋；恶意服务器偏离协议；时间等元数据。", ACCENT2),
    ]
    for title, body, color in cards:
        rounded(d, (1110, box_y, 1745, box_y + 155), WHITE, outline=LINE, radius=24)
        d.text((1150, box_y + 28), title, font=F_H3, fill=color)
        draw_wrapped(d, (1150, box_y + 76), body, F_SMALL, INK, max_width=540)
        box_y += 185
    return img


def slide5():
    img, d, _, _ = base("矩阵分解直觉：把评分矩阵压成低秩表示", "Matrix Factorization")
    # Matrix U
    d.text((145, 215), "用户 × 物品评分矩阵 U", font=F_H3, fill=INK)
    x0, y0 = 140, 285
    for i in range(9):
        for j in range(11):
            val = (i * 7 + j * 11) % 10
            fill = (236, 231, 222) if val < 6 else (88, 134, 166)
            if val in (0, 1, 2):
                fill = (248, 246, 241)
            d.rectangle((x0 + j * 45, y0 + i * 42, x0 + j * 45 + 36, y0 + i * 42 + 33), fill=fill, outline=WHITE)
    d.text((690, 450), "≈", font=font(78, "bold"), fill=ACCENT2)
    rounded(d, (830, 290, 1060, 680), (229, 236, 231), outline=LINE, radius=16)
    d.text((875, 705), "A：用户嵌入", font=F_SMALL_B, fill=MUTED)
    for i in range(9):
        for j in range(3):
            d.rectangle((860 + j * 54, 325 + i * 36, 860 + j * 54 + 44, 325 + i * 36 + 26), fill=(76 + j * 34, 126, 145), outline=WHITE)
    d.text((1105, 450), "×", font=font(62, "bold"), fill=ACCENT2)
    rounded(d, (1235, 340, 1695, 550), (236, 229, 218), outline=LINE, radius=16)
    d.text((1350, 575), "B：物品嵌入 / 模型", font=F_SMALL_B, fill=MUTED)
    for i in range(3):
        for j in range(9):
            d.rectangle((1270 + j * 42, 375 + i * 46, 1270 + j * 42 + 32, 375 + i * 46 + 34), fill=(176, 82 + i * 28, 80 + j * 8), outline=WHITE)
    rounded(d, (180, 800, 1700, 920), WHITE, outline=LINE, radius=24)
    draw_wrapped(
        d,
        (230, 827),
        "训练阶段输出低维 item embeddings B；服务阶段，用户 i 的推荐分数可看作 u(i) · BᵀB，推荐最高且未看过的物品。",
        F_BODY,
        INK,
        max_width=1420,
    )
    return img


def slide6():
    img, d, _, _ = base("算法步骤：Power Iteration 做私有矩阵分解", "Algorithm")
    add_paper_crop(img, 7, (0.05, 0.05, 0.56, 0.37), (86, 204, 840, 610), caption="论文 Figure 4：矩阵分解 via power iteration")
    steps = [
        ("初始化", "B 置零；对第 i 个 latent factor 随机初始化向量 v"),
        ("正交化", "SetOrthogonal(v, B)：从 v 中减去已求行向量方向的分量"),
        ("幂迭代", "重复 ℓ 次：v ← Uᵀ(Uv)，再正交化并归一化"),
        ("写入模型", "将收敛后的 v 作为 B 的第 i 行；继续提取下一个因子"),
        ("线性恢复", "最后计算 A := U·Bᵀ；推荐分数来自 A·B"),
    ]
    y = 215
    for idx, (title, body) in enumerate(steps):
        color = ACCENT if idx < 3 else ACCENT2
        d.ellipse((960, y + 4, 1012, y + 56), fill=color)
        d.text((978, y + 17), str(idx + 1), font=F_SMALL_B, fill=WHITE)
        d.text((1040, y), title, font=F_H3, fill=INK)
        draw_wrapped(d, (1040, y + 42), body, F_SMALL, MUTED, max_width=650)
        if idx < len(steps) - 1:
            arrow(d, (986, y + 70), (986, y + 102), fill=LINE, width=4)
        y += 135
    return img


def slide7():
    img, d, _, _ = base("为什么这个算法适合 3PC？", "MPC Co-design")
    rounded(d, (110, 240, 510, 770), WHITE, outline=LINE, radius=26)
    d.text((160, 288), "线性大块", font=F_H2, fill=ACCENT)
    draw_wrapped(d, (160, 350), "矩阵-向量乘法：U·v、Uᵀ·v\n\nReplicated secret sharing 可本地/非交互高速完成，主成本接近明文 3×。", F_BODY, INK, 290)
    arrow(d, (540, 505), (700, 505), fill=ACCENT, width=5)
    rounded(d, (730, 240, 1130, 770), WHITE, outline=LINE, radius=26)
    d.text((780, 288), "非线性小块", font=F_H2, fill=ACCENT2)
    draw_wrapped(d, (780, 350), "截断 Truncₜ：乘法后恢复固定点精度\n\n归一化 Normalize：计算 1/||v||，防止溢出/下溢。", F_BODY, INK, 290)
    arrow(d, (1160, 505), (1320, 505), fill=ACCENT, width=5)
    rounded(d, (1350, 240, 1780, 770), (31, 39, 51), radius=26)
    d.text((1400, 288), "Nudge 优化", font=F_H2, fill=WHITE)
    draw_wrapped(d, (1400, 350), "FSS 处理比较/最高有效位\n\n截断通信降低约 6×\n\n归一化用 Newton-Raphson，默认 5 轮。", F_BODY, (226, 231, 237), 320)
    rounded(d, (230, 850, 1680, 940), (234, 228, 218), radius=22)
    draw_wrapped(d, (270, 872), "核心 co-design：把算法写成“矩阵-向量程序”，让大规模线性代数尽量不通信，只在必要的非线性函数上支付 3PC 成本。", F_BODY_B, INK, 1360)
    return img


def slide8():
    img, d, _, _ = base("复杂度叙事：通信从“跟评分数走”变成“跟用户+物品走”", "Scalability")
    d.text((118, 230), "固定 embedding 维度 d 时：", font=F_H2, fill=INK)
    rounded(d, (125, 310, 820, 470), WHITE, outline=LINE, radius=26)
    d.text((170, 350), "Compute", font=F_H3, fill=ACCENT)
    d.text((390, 338), "O(mn)", font=font(56, "bold"), fill=INK)
    d.text((170, 420), "矩阵 U 是密态/secret-shared，看不到稀疏结构", font=F_SMALL, fill=MUTED)
    rounded(d, (125, 530, 820, 690), WHITE, outline=LINE, radius=26)
    d.text((170, 570), "Communication", font=F_H3, fill=ACCENT2)
    d.text((460, 558), "O(m+n)", font=font(56, "bold"), fill=INK)
    d.text((170, 640), "显著区别于许多通信随 ratings 数增长的方案", font=F_SMALL, fill=MUTED)
    # mini comparison chart
    x, y = 1030, 250
    d.text((x, y), "直觉对比", font=F_H2, fill=INK)
    labels = ["传统私有 MF", "Nudge"]
    vals = [0.92, 0.18]
    for i, (lab, val) in enumerate(zip(labels, vals)):
        yy = y + 115 + i * 150
        d.text((x, yy), lab, font=F_BODY_B, fill=INK)
        rounded(d, (x, yy + 48, x + 650, yy + 84), (232, 226, 216), radius=18)
        rounded(d, (x, yy + 48, x + int(650 * val), yy + 84), ACCENT2 if i == 0 else ACCENT, radius=18)
        d.text((x + 680, yy + 44), "通信量级", font=F_SMALL, fill=MUTED)
    rounded(d, (1030, 760, 1730, 900), (31, 39, 51), radius=26)
    draw_wrapped(d, (1080, 795), "代价：计算仍是 O(mn)。当用户和物品都极大时，需要随机投影等降维扩展。", F_BODY_B, WHITE, 610)
    return img


def slide9():
    img, d, _, _ = base("推荐质量：接近明文矩阵分解", "Evaluation · Quality")
    add_paper_crop(img, 9, (0.05, 0.05, 0.62, 0.36), (90, 205, 920, 615), caption="论文 Figure 5：Netflix 数据集上的 Recall / nDCG 对比")
    # Redrawn key numbers
    d.text((1050, 230), "关键读数（k=20）", font=F_H2, fill=INK)
    metrics = [("Nudge nDCG@20", "0.29", ACCENT), ("MultVAE nDCG@20", "0.31", ACCENT2), ("Nudge Recall@20", "0.23", GREEN)]
    y = 320
    for label, val, color in metrics:
        rounded(d, (1050, y, 1680, y + 95), WHITE, outline=LINE, radius=22)
        d.text((1090, y + 27), label, font=F_SMALL_B, fill=MUTED)
        d.text((1500, y + 15), val, font=font(48, "bold"), fill=color)
        y += 125
    rounded(d, (1050, 735, 1710, 890), (234, 228, 218), radius=24)
    draw_wrapped(d, (1090, 765), "结论：固定步数幂迭代、固定点算术、延迟截断和近似归一化，没有显著伤害推荐质量。", F_BODY_B, INK, 580)
    return img


def slide10():
    img, d, _, _ = base("性能：私有矩阵分解从“天/小时”压到“秒/分钟”", "Evaluation · Performance")
    add_paper_crop(img, 10, (0.03, 0.05, 0.97, 0.43), (90, 185, 1780, 560), caption="论文 Table 6：Nudge 与 prior private MF / MP-SPDZ baseline 对比")
    highlights = [
        ("MovieLens100K", "10.7 sec LAN / 51.8 sec WAN", "0.5 GB"),
        ("MovieLens1M", "32.8 sec LAN / 1.8 min WAN", "1.4 GB"),
        ("Netflix", "50.2 min LAN / 1h13 WAN", "38.9 GB"),
    ]
    x = 112
    for name, time, comm in highlights:
        rounded(d, (x, 685, x + 520, 850), WHITE, outline=LINE, radius=24)
        d.text((x + 34, 722), name, font=F_H3, fill=ACCENT)
        d.text((x + 34, 770), time, font=F_SMALL_B, fill=INK)
        d.text((x + 34, 810), f"server-to-server: {comm}", font=F_SMALL, fill=MUTED)
        x += 585
    d.text((112, 930), "论文强调：相比 garbled-circuit 系统，通信和计算低四个数量级；相比 PIRSONA，通信少 8× 且少 1 个非串谋方。", font=F_SMALL_B, fill=ACCENT2)
    return img


def slide11():
    img, d, _, _ = base("端到端与横向扩展：训练重，服务轻", "Evaluation · Scale")
    add_paper_crop(img, 11, (0.05, 0.05, 0.96, 0.34), (88, 190, 1120, 560), caption="论文 Figure 7：百万级用户/物品下的扩展趋势")
    add_paper_crop(img, 12, (0.05, 0.05, 0.56, 0.33), (1180, 190, 1760, 560), caption="论文 Table 8：Netflix 上服务端到端开销")
    cards = [
        ("Criteo", "6.1M users / 700 items", "1.5 h · 125 GB"),
        ("Yelp", "279K users / 148K items", "8 h · 248 GB"),
        ("Serving", "Netflix user fetch", "298 KB · 0.38 s"),
    ]
    x = 160
    for title, mid, bottom in cards:
        rounded(d, (x, 705, x + 470, 850), (31, 39, 51), radius=24)
        d.text((x + 36, 734), title, font=F_H3, fill=WHITE)
        d.text((x + 36, 782), mid, font=F_SMALL, fill=(203, 210, 220))
        d.text((x + 36, 815), bottom, font=F_SMALL_B, fill=(238, 177, 146))
        x += 560
    d.text((160, 930), "读法：训练阶段是重计算；但模型训练好后，用户拉取推荐只需数百 KB、亚秒级延迟。", font=F_SMALL_B, fill=INK)
    return img


def slide12():
    img, d, _, _ = base("局限、扩展与汇报结论", "Takeaways")
    cols = [
        ("局限", [
            "需要 3 个非串谋服务器，只容忍 1 个半诚实 compromise",
            "不负责私有内容获取；需和 PIR / 匿名代理配合",
            "最终模型 B 仍可能编码聚合用户行为信息",
            "训练成本显著高于明文系统，且密态矩阵无法利用稀疏性",
        ], ACCENT2),
        ("扩展", [
            "随机投影：先把 U·S 压到低维 sketch，再做私有矩阵分解",
            "差分隐私：对 UᵀU 加高斯噪声，Netflix nDCG@20 从 0.29 降到 0.24",
            "输入有效性检查：验证评分在 {0,1}，几乎免费",
            "可迁移到 PCA、PageRank、谱聚类、低维 embedding 等任务",
        ], ACCENT),
        ("一句话评价", [
            "Nudge 的核心贡献不是“更复杂的推荐模型”，而是选择了适合 MPC 的线性代数算法",
            "通过 power iteration + 3PC/FSS 原语 co-design，把私有推荐推进到 Netflix 量级",
            "最适合讲作：算法结构如何决定密码协议成本",
        ], GREEN),
    ]
    x = 88
    for title, items, color in cols:
        rounded(d, (x, 205, x + 545, 855), WHITE, outline=LINE, radius=28)
        d.text((x + 40, 250), title, font=F_H2, fill=color)
        bullet_list(d, x + 42, 320, items, 455, f=F_SMALL, fill=INK, gap=16, dot=color)
        x += 595
    d.text((92, 936), "开放问题：恶意安全、多方数扩展、仅 2 个非串谋服务器且不牺牲性能。", font=F_SMALL_B, fill=ACCENT2)
    return img


SLIDES = [
    slide1,
    slide2,
    slide3,
    slide4,
    slide5,
    slide6,
    slide7,
    slide8,
    slide9,
    slide10,
    slide11,
    slide12,
]


PLAN = {
    "title": "Nudge: A Private Recommendations Engine",
    "style": "minimal-swiss / editorial academic",
    "style_guidelines": {
        "color_palette": "Warm paper background, dark ink text, muted blue and terracotta accents",
        "typography": "Clean sans-serif Chinese/English hierarchy; large titles, restrained body copy",
        "imagery": "Paper figures cropped from the PDF, hand-redrawn architecture and metric diagrams",
        "layout": "Editorial academic briefing, strong whitespace, 1–2 focal elements per slide",
    },
    "aspect_ratio": "16:9",
    "slides": [
        {"slide_number": 1, "type": "title", "title": "Nudge: A Private Recommendations Engine"},
        {"slide_number": 2, "type": "motivation", "title": "为什么需要 Nudge？"},
        {"slide_number": 3, "type": "architecture", "title": "系统总览：三服务器拆分信任"},
        {"slide_number": 4, "type": "security", "title": "威胁模型：强隐私，但不是万能"},
        {"slide_number": 5, "type": "concept", "title": "矩阵分解直觉：把评分矩阵压成低秩表示"},
        {"slide_number": 6, "type": "algorithm", "title": "算法步骤：Power Iteration 做私有矩阵分解"},
        {"slide_number": 7, "type": "algorithm", "title": "为什么这个算法适合 3PC？"},
        {"slide_number": 8, "type": "analysis", "title": "复杂度叙事：通信从跟评分数走变成跟用户+物品走"},
        {"slide_number": 9, "type": "experiment", "title": "推荐质量：接近明文矩阵分解"},
        {"slide_number": 10, "type": "experiment", "title": "性能：私有矩阵分解从天/小时压到秒/分钟"},
        {"slide_number": 11, "type": "experiment", "title": "端到端与横向扩展：训练重，服务轻"},
        {"slide_number": 12, "type": "conclusion", "title": "局限、扩展与汇报结论"},
    ],
}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    with open(WORK / "nudge-plan.json", "w", encoding="utf-8") as f:
        json.dump(PLAN, f, ensure_ascii=False, indent=2)
    for idx, fn in enumerate(SLIDES, start=1):
        img = fn()
        path = OUT / f"nudge-slide-{idx:02d}.jpg"
        img.save(path, quality=94)
        print(path)


if __name__ == "__main__":
    main()
