from pathlib import Path
from zipfile import ZipFile

from pptx import Presentation
from pptx.dml.color import RGBColor
from pptx.enum.shapes import MSO_CONNECTOR, MSO_SHAPE
from pptx.enum.text import MSO_ANCHOR, PP_ALIGN
from pptx.util import Inches, Pt


ROOT = Path("/Users/bytedance/PCA/output/nudge-paper2ppt")
FIG = ROOT / "assets" / "figures"
OUT = ROOT / "final_presentation_cn.pptx"
QA = ROOT / "qa_report.md"

W, H = 13.333, 7.5

COLORS = {
    "ink": RGBColor(28, 33, 42),
    "muted": RGBColor(92, 101, 117),
    "light": RGBColor(247, 248, 250),
    "line": RGBColor(210, 216, 225),
    "blue": RGBColor(42, 92, 170),
    "blue2": RGBColor(227, 237, 251),
    "green": RGBColor(38, 122, 92),
    "green2": RGBColor(226, 242, 235),
    "amber": RGBColor(164, 112, 24),
    "amber2": RGBColor(250, 240, 220),
    "red": RGBColor(168, 62, 62),
    "red2": RGBColor(249, 226, 226),
    "white": RGBColor(255, 255, 255),
}


def cm(v):
    return Inches(v)


def set_run(run, size=14, color="ink", bold=False, font="Microsoft YaHei"):
    run.font.name = font
    run.font.size = Pt(size)
    run.font.bold = bold
    run.font.color.rgb = COLORS[color]


def add_text(slide, x, y, w, h, text, size=14, color="ink", bold=False, align=PP_ALIGN.LEFT):
    box = slide.shapes.add_textbox(cm(x), cm(y), cm(w), cm(h))
    tf = box.text_frame
    tf.clear()
    tf.word_wrap = True
    tf.margin_left = cm(0.03)
    tf.margin_right = cm(0.03)
    tf.margin_top = cm(0.02)
    tf.margin_bottom = cm(0.02)
    p = tf.paragraphs[0]
    p.alignment = align
    p.line_spacing = 1.08
    r = p.add_run()
    r.text = text
    set_run(r, size=size, color=color, bold=bold)
    return box


def add_multiline(slide, x, y, w, h, lines, size=13, color="ink", bullet=False):
    box = slide.shapes.add_textbox(cm(x), cm(y), cm(w), cm(h))
    tf = box.text_frame
    tf.clear()
    tf.word_wrap = True
    tf.margin_left = cm(0.06)
    tf.margin_right = cm(0.06)
    tf.margin_top = cm(0.04)
    tf.margin_bottom = cm(0.04)
    for i, line in enumerate(lines):
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        p.text = line
        p.level = 0
        p.line_spacing = 1.08
        if bullet:
            p.text = "• " + line
        for run in p.runs:
            set_run(run, size=size, color=color)
    return box


def add_title(slide, title, subtitle=None):
    add_text(slide, 0.55, 0.32, 11.7, 0.55, title, size=24, bold=True)
    if subtitle:
        add_text(slide, 0.58, 0.9, 11.7, 0.32, subtitle, size=10.5, color="muted")


def add_source(slide, text):
    add_text(slide, 0.58, 7.08, 12.2, 0.2, text, size=7.5, color="muted")


def add_note(slide, note):
    notes = slide.notes_slide.notes_text_frame
    notes.clear()
    notes.text = note


def rect(slide, x, y, w, h, text="", fill="white", line="line", radius=False, size=12, bold=False):
    shape_type = MSO_SHAPE.ROUNDED_RECTANGLE if radius else MSO_SHAPE.RECTANGLE
    s = slide.shapes.add_shape(shape_type, cm(x), cm(y), cm(w), cm(h))
    s.fill.solid()
    s.fill.fore_color.rgb = COLORS[fill]
    s.line.color.rgb = COLORS[line]
    s.line.width = Pt(1)
    if text:
        tf = s.text_frame
        tf.clear()
        tf.vertical_anchor = MSO_ANCHOR.MIDDLE
        tf.word_wrap = True
        p = tf.paragraphs[0]
        p.alignment = PP_ALIGN.CENTER
        p.text = text
        for run in p.runs:
            set_run(run, size=size, color="ink", bold=bold)
    return s


def arrow(slide, x1, y1, x2, y2, color="blue"):
    c = slide.shapes.add_connector(MSO_CONNECTOR.STRAIGHT, cm(x1), cm(y1), cm(x2), cm(y2))
    c.line.color.rgb = COLORS[color]
    c.line.width = Pt(1.6)
    c.line.end_arrowhead = True
    return c


def add_img(slide, path, x, y, w, h):
    from PIL import Image

    img = Image.open(path)
    iw, ih = img.size
    box_ratio = w / h
    img_ratio = iw / ih
    if img_ratio > box_ratio:
        width = w
        height = w / img_ratio
        left = x
        top = y + (h - height) / 2
    else:
        height = h
        width = h * img_ratio
        top = y
        left = x + (w - width) / 2
    return slide.shapes.add_picture(str(path), cm(left), cm(top), width=cm(width), height=cm(height))


def add_takeaway(slide, text, tone="blue"):
    fill = {"blue": "blue2", "green": "green2", "amber": "amber2", "red": "red2"}.get(tone, "blue2")
    border = {"blue": "blue", "green": "green", "amber": "amber", "red": "red"}.get(tone, "blue")
    rect(slide, 0.58, 6.55, 12.2, 0.42, text, fill=fill, line=border, radius=True, size=11.5, bold=True)


def new_slide(prs, title, subtitle=None):
    slide = prs.slides.add_slide(prs.slide_layouts[6])
    bg = slide.background
    bg.fill.solid()
    bg.fill.fore_color.rgb = COLORS["white"]
    add_title(slide, title, subtitle)
    return slide


def build():
    prs = Presentation()
    prs.slide_width = cm(W)
    prs.slide_height = cm(H)

    # 1 Cover
    s = prs.slides.add_slide(prs.slide_layouts[6])
    s.background.fill.solid()
    s.background.fill.fore_color.rgb = COLORS["white"]
    rect(s, 0.55, 0.65, 0.08, 5.9, fill="blue", line="blue")
    add_text(s, 0.85, 0.8, 10.7, 0.45, "论文精读汇报", size=15, color="muted")
    add_text(s, 0.85, 1.34, 11.6, 1.15, "Nudge: A Private\nRecommendations Engine", size=31, bold=True)
    add_text(s, 0.88, 2.75, 10.9, 0.36, "三方安全计算驱动的隐私保护推荐系统", size=16, color="blue", bold=True)
    add_multiline(
        s,
        0.9,
        4.45,
        7.8,
        0.7,
        [
            "Alexandra Henzinger · Emma Dauterman · Henry Corrigan-Gibbs · Dan Boneh",
            "MIT / Stanford · USENIX Security · 代码开源：Zenodo 17968761",
        ],
        size=10.5,
        color="muted",
    )
    rect(s, 9.15, 3.5, 3.2, 1.7, "一句话：\n把推荐训练放进 3PC，\n只泄露聚合模型本身。", fill="blue2", line="blue", radius=True, size=14, bold=True)
    add_note(s, "开场强调：这不是简单把现有推荐算法套进 MPC，而是把矩阵分解算法和三方计算协议协同设计。")

    # 2 Background
    s = new_slide(prs, "推荐系统越个性化，越需要处理敏感行为轨迹", "研究背景与待解决问题")
    add_multiline(s, 0.7, 1.55, 4.3, 1.7, ["点击、观看、评分暴露偏好", "传统平台直接持有明文日志", "数据转卖、泄露与滥用风险真实存在"], size=14, bullet=True)
    rect(s, 5.35, 1.4, 1.8, 0.75, "匿名代理", fill="light", radius=True, size=13)
    rect(s, 7.35, 1.4, 1.8, 0.75, "联邦学习", fill="light", radius=True, size=13)
    rect(s, 9.35, 1.4, 1.8, 0.75, "可信硬件", fill="light", radius=True, size=13)
    rect(s, 5.35, 2.55, 5.8, 0.88, "仍可能泄露中间模型、身份链路或受侧信道攻击", fill="red2", line="red", radius=True, size=13, bold=True)
    arrow(s, 6.2, 2.15, 7.05, 2.55, "red")
    arrow(s, 8.25, 2.15, 8.25, 2.55, "red")
    arrow(s, 10.1, 2.15, 9.45, 2.55, "red")
    add_text(s, 0.75, 4.55, 11.5, 0.55, "Nudge 的目标不是隐藏模型存在，而是让服务器在训练与推荐过程中看不到任何单个用户的偏好明文。", size=17, bold=True)
    add_takeaway(s, "核心瓶颈：已有私密推荐系统规模太小；已有大规模推荐系统隐私太弱。", "amber")
    add_note(s, "讲解时可以用“平台知道你看了什么、喜欢什么、没点什么”切入；Nudge 的定位是密码学隐私而非单纯合规匿名化。")

    # 3 Claim/security
    s = new_slide(prs, "Nudge 的安全承诺：一台服务器被攻陷仍看不到用户偏好", "系统目标、泄露边界与非目标")
    rect(s, 0.85, 1.45, 3.4, 1.15, "保护对象\n用户评分 / 交互行为", fill="green2", line="green", radius=True, size=15, bold=True)
    rect(s, 4.95, 1.45, 3.4, 1.15, "信任拆分\n3 台独立服务器", fill="blue2", line="blue", radius=True, size=15, bold=True)
    rect(s, 9.05, 1.45, 3.4, 1.15, "威胁模型\n半诚实 + 攻陷 1 台", fill="amber2", line="amber", radius=True, size=15, bold=True)
    add_multiline(s, 1.0, 3.25, 5.45, 1.45, ["服务器学习：聚合推荐模型 B", "服务器学习：每个用户提交了多少评分", "服务器不学习：用户给哪些 item 评分"], size=14, bullet=True)
    add_multiline(s, 7.05, 3.25, 5.4, 1.45, ["不防恶意服务器偏离协议", "不防 2 台或更多服务器串谋", "不负责 item 的私密获取，需要 PIR / Tor / Private Relay 等配合"], size=14, bullet=True)
    add_takeaway(s, "它把“平台必须看见用户日志才能推荐”的默认假设，改成了“只看聚合模型”。", "green")
    add_note(s, "这里要清楚区分：Nudge 的安全不是零泄露，模型本身仍可能编码聚合行为，所以论文后面讨论差分隐私作为补充。")

    # 4 Protocol flow
    s = new_slide(prs, "协议层面只有三件事：收集、训练、返回推荐", "Protocol flow")
    add_img(s, FIG / "fig1_protocol_visual.png", 0.7, 1.35, 3.3, 2.55)
    steps = [
        ("1", "Private data collection", "用户把 rating vector\n秘密分享给三台服务器"),
        ("2", "Private matrix factorization", "服务器在 3PC 中训练\nA（用户嵌入）与 B（物品嵌入）"),
        ("3", "Private recommendation", "返回用户个性化分数的\nsecret shares，用户本地恢复"),
    ]
    x = 4.4
    for idx, (num, title, body) in enumerate(steps):
        rect(s, x + idx * 2.85, 1.55, 2.35, 1.75, f"{num}. {title}\n{body}", fill="light", line="blue", radius=True, size=10.8, bold=True)
        if idx < 2:
            arrow(s, x + idx * 2.85 + 2.35, 2.42, x + (idx + 1) * 2.85, 2.42)
    add_multiline(s, 4.55, 4.25, 7.6, 1.2, ["A 是每个用户的 embedding，仍为 secret-shared", "B 是 item embedding，可公开给服务器用于后续推荐", "用户最终只恢复自己的推荐分数"], size=13, bullet=True)
    add_source(s, "Source: Fig. 1 and Section 3.2, Nudge")
    add_note(s, "建议用这页建立全局地图：后面所有密码学细节都服务于第二步的 private matrix factorization。")

    # 5 MPC primitives
    s = new_slide(prs, "技术核心：把 power iteration 写成 3PC 友好的 matrix-vector program", "Secure computation primitives")
    add_img(s, FIG / "fig2_matrix_vector_program.png", 0.75, 1.25, 5.65, 1.85)
    add_img(s, FIG / "table3_3pc_functions.png", 6.75, 1.2, 5.75, 2.6)
    add_multiline(s, 0.9, 3.85, 5.45, 1.3, ["线性部分：RSS 支持密态矩阵-向量乘", "非线性部分：FSS 支持 truncation / normalization", "通信随用户数和 item 数线性增长"], size=13.5, bullet=True)
    add_multiline(s, 7.0, 4.25, 5.25, 1.05, ["新贡献：3PC truncation 通信约 6× 改进", "normalization 用 MSB + Newton-Raphson 找近似缩放因子"], size=13.5, bullet=True)
    add_takeaway(s, "抽象关键：只要算法可分解为“矩阵乘 + 简单非线性”，就能高效放进 3PC。", "blue")
    add_source(s, "Source: Fig. 2, Table 3, Sections 4.1-4.3, Nudge")
    add_note(s, "解释 RSS 与 FSS 不需要深入公式：RSS 让三方可以非交互地做线性代数；FSS 让比较、截断等非线性门以低通信完成。")

    # 6 Algorithm overview with Fig 4
    s = new_slide(prs, "矩阵分解算法：用 power iteration 逐个提取 item embeddings", "Algorithm core")
    add_img(s, FIG / "fig4_power_iteration_algorithm.png", 0.75, 1.05, 4.65, 5.35)
    add_multiline(s, 5.75, 1.35, 6.35, 1.35, ["目标：把评分矩阵 U 分解为 A·B", "B 的每一行是 UᵀU 的 top eigenvector", "A = U·Bᵀ 是最后一步线性计算"], size=15, bullet=True)
    rect(s, 5.9, 3.35, 5.95, 1.15, "为什么不是梯度下降？\npower iteration 每次产出 B 的一行；学到的 B 可以公开，反过来帮助后续计算。", fill="blue2", line="blue", radius=True, size=13.5, bold=True)
    add_multiline(s, 5.95, 5.0, 5.7, 0.8, ["固定点数值中每轮重正交化，避免已找到方向污染下一主方向。"], size=13.5, bullet=True)
    add_source(s, "Source: Fig. 4 and Section 5.1, Nudge")
    add_note(s, "这页不要逐行念伪代码；先讲 PCA/矩阵分解直觉：找到 item 空间里的主要方向，再用这些方向表达每个用户。")

    # 7 Detailed algorithm
    s = new_slide(prs, "算法步骤拆解：每个主方向都经历“随机起点 → 反复拉伸 → 归一化”", "Matrix factorization via power iteration")
    labels = [
        ("① 初始化", "B 置零；为第 i 行随机采样 v"),
        ("② 正交化", "v ← SetOrthogonal(v, B)"),
        ("③ 幂迭代", "v ← Uᵀ(Uv)，沿最大特征方向被拉伸"),
        ("④ 截断", "固定点乘法后执行 lazy truncation"),
        ("⑤ 归一化", "v ← Normalize(v)，控制溢出/下溢"),
        ("⑥ 写入 B", "把 v 作为 B 的第 i 行公开"),
        ("⑦ 得到 A", "A ← U·Bᵀ，仍为 secret-shared"),
    ]
    coords = [(0.7, 1.35), (3.6, 1.35), (6.5, 1.35), (9.4, 1.35), (2.2, 4.1), (5.2, 4.1), (8.2, 4.1)]
    for i, ((head, body), (x, y)) in enumerate(zip(labels, coords)):
        fill = "blue2" if i in [2, 4] else "light"
        rect(s, x, y, 2.35, 1.05, f"{head}\n{body}", fill=fill, line="blue", radius=True, size=11.2, bold=True)
    for i in range(3):
        arrow(s, coords[i][0] + 2.35, coords[i][1] + 0.52, coords[i+1][0], coords[i+1][1] + 0.52)
    arrow(s, 10.55, 2.4, 3.35, 4.1)
    arrow(s, 4.55, 4.63, 5.2, 4.63)
    arrow(s, 7.55, 4.63, 8.2, 4.63)
    add_text(s, 0.85, 6.15, 11.7, 0.42, "循环结构：外层重复 d 次得到 d 个 embedding 方向；内层重复 ℓ 次让 v 收敛到当前最大特征方向。", size=14.5, bold=True, color="blue")
    add_note(s, "这一页详细讲算法。强调 UᵀU 不需要显式公开；服务器只在 secret shares 上做矩阵-向量程序。公开 B 是设计选择，因为 B 是最终聚合模型的一部分。")

    # 8 Privacy execution
    s = new_slide(prs, "如何把算法放进 3PC：线性步骤本地快跑，非线性步骤低通信交互", "From algorithm to secure protocol")
    rect(s, 0.8, 1.45, 3.2, 1.1, "Mul(U, v)\nMul(Uᵀ, ·)", fill="green2", line="green", radius=True, size=15, bold=True)
    rect(s, 5.05, 1.45, 3.2, 1.1, "Truncate\n固定点缩放", fill="amber2", line="amber", radius=True, size=15, bold=True)
    rect(s, 9.3, 1.45, 3.2, 1.1, "Normalize\nL2 归一化", fill="blue2", line="blue", radius=True, size=15, bold=True)
    arrow(s, 4.0, 2.0, 5.05, 2.0)
    arrow(s, 8.25, 2.0, 9.3, 2.0)
    add_multiline(s, 0.95, 3.25, 3.05, 1.2, ["RSS：三方持有 replicated shares", "矩阵-向量乘可非交互完成"], size=12.8, bullet=True)
    add_multiline(s, 5.2, 3.25, 3.05, 1.2, ["乘法后恢复 fixed-point 格式", "Nudge 协议比 prior truncation 低约 6× 通信"], size=12.8, bullet=True)
    add_multiline(s, 9.45, 3.25, 3.05, 1.2, ["MSB 给出初始缩放估计", "Newton-Raphson 常数轮近似"], size=12.8, bullet=True)
    add_takeaway(s, "Theorem 5.1：三方可安全计算 A 和 B，通信量随 d、ℓ、m+n 线性主导。", "green")
    add_source(s, "Source: Theorem 4.2, Lemma 4.3/4.4, Theorem 5.1, Nudge")
    add_note(s, "这一页是技术桥梁：算法的每个操作都要落到 3PC 原语。不要展开大 O 公式，讲“线性代数占计算，非线性门决定通信轮数”。")

    # 9 Evaluation setup
    s = new_slide(prs, "实验设计覆盖三件事：推荐质量、私密训练成本、扩展规模", "Evaluation setup")
    rect(s, 0.75, 1.35, 3.55, 1.4, "推荐质量\nNetflix Prize\nRecall@k / nDCG@k", fill="blue2", line="blue", radius=True, size=14, bold=True)
    rect(s, 4.9, 1.35, 3.55, 1.4, "私密训练成本\nMovieLens / Netflix\nLAN / WAN", fill="green2", line="green", radius=True, size=14, bold=True)
    rect(s, 9.05, 1.35, 3.55, 1.4, "规模扩展\nCriteo / Yelp / Steam\nPinterest / MIND", fill="amber2", line="amber", radius=True, size=14, bold=True)
    add_multiline(s, 0.85, 3.45, 5.5, 1.55, ["基线：TopPopular、ItemKNN、GraphWalk、ALS、SVD、MultVAE", "测试集：10,000 users；每人 80% rating 训练，20% 作 ground truth"], size=13.5, bullet=True)
    add_multiline(s, 7.0, 3.45, 5.3, 1.55, ["主实验机器：3 台 192-core servers", "Netflix：463,435 users、17,769 movies、56M ratings"], size=13.5, bullet=True)
    add_takeaway(s, "论文不只问“能否安全”，还问“推荐质量是否保住、规模是否接近真实业务”。", "blue")
    add_note(s, "注意提醒：Nudge 与非隐私 SVD 的成本不是同一量级，但论文关注的是在强隐私下能否达到实用规模。")

    # 10 Quality result
    s = new_slide(prs, "推荐质量基本追平明文矩阵分解，只略低于神经推荐", "Main result: recommendation quality")
    add_img(s, FIG / "fig5_recommendation_quality.png", 0.65, 1.25, 6.25, 4.65)
    rect(s, 7.3, 1.35, 2.35, 1.0, "nDCG@20\n0.29", fill="blue2", line="blue", radius=True, size=19, bold=True)
    rect(s, 10.0, 1.35, 2.35, 1.0, "Recall@20\n0.23", fill="green2", line="green", radius=True, size=19, bold=True)
    add_multiline(s, 7.25, 3.0, 5.05, 1.8, ["Nudge 与 ALS / SVD 持平", "优于 TopPopular / ItemKNN / GraphWalk", "距离 MultVAE 约 12%"], size=15, bullet=True)
    add_takeaway(s, "固定点、近似 normalization 和有限轮 power iteration 没有明显损害推荐质量。", "green")
    add_source(s, "Source: Fig. 5 and Section 7.1, Nudge")
    add_note(s, "这里重点讲 nDCG@20=0.29 这个数字：它与明文矩阵分解相当，说明隐私计算带来的主要代价在系统成本，而不是推荐质量。")

    # 11 Table 6 performance
    s = new_slide(prs, "私密矩阵分解：Netflix 全量训练 50 分钟，通信 38.9 GB", "Main result: private matrix factorization")
    add_img(s, FIG / "table6_private_mf_performance.png", 0.55, 1.12, 12.15, 3.75)
    rect(s, 0.9, 5.15, 2.7, 0.75, "Netflix\n50.2 min", fill="blue2", line="blue", radius=True, size=15, bold=True)
    rect(s, 3.95, 5.15, 2.7, 0.75, "server-server\n38.9 GB", fill="green2", line="green", radius=True, size=15, bold=True)
    rect(s, 7.0, 5.15, 2.7, 0.75, "MovieLens1M\n32.8 sec", fill="amber2", line="amber", radius=True, size=15, bold=True)
    rect(s, 10.05, 5.15, 2.7, 0.75, "WAN Netflix\n1h13", fill="light", line="line", radius=True, size=15, bold=True)
    add_takeaway(s, "相比 prior private recommender，Nudge 的通信和计算下降多个数量级；代价是需要 3 台不串谋服务器。", "blue")
    add_source(s, "Source: Table 6 and Section 7.2, Nudge")
    add_note(s, "表 6 很密，只需要讲高亮行。MovieLens1M 相比 GraphSC 从估计 11 天到 32.8 秒，是论文最有冲击力的对比之一。")

    # 12 Scaling
    s = new_slide(prs, "规模扩展：Nudge 可处理百万用户或十万级 item 的矩阵", "Scaling to larger datasets")
    add_img(s, FIG / "fig7_scaling_curves.png", 0.55, 1.15, 7.25, 3.15)
    add_multiline(s, 8.1, 1.35, 4.55, 2.45, ["Criteo：6.1M users / 700 ads，1.5 h，125 GB", "Yelp：279K users / 148K businesses，8 h，248 GB", "Steam：2.5M users / 15K games，4.7 h，78 GB", "Microsoft News：50K users / 160K articles，3 h，263 GB"], size=12.5, bullet=True)
    add_multiline(s, 0.9, 5.0, 11.6, 0.8, ["理论趋势：计算约 O(mn)，通信约 O(m+n)；实际通信在 n 足够大时主要被 item 数支配。"], size=14, bullet=True)
    add_source(s, "Source: Fig. 7 and Section 7.3, Nudge")
    add_note(s, "这页讲规模边界。Nudge 可以到很大，但它看不到稀疏结构，因此计算随 m×n 增长，这也是后续 dimensionality reduction 的动机。")

    # 13 End-to-end + extensions
    s = new_slide(prs, "端到端成本显示：训练贵，但在线推荐可以做到秒级", "Serving recommendations and extensions")
    add_img(s, FIG / "table8_end_to_end_costs.png", 0.65, 1.22, 5.45, 3.5)
    rect(s, 6.75, 1.35, 2.55, 0.82, "Logging\n694 bytes", fill="light", radius=True, size=14, bold=True)
    rect(s, 9.65, 1.35, 2.55, 0.82, "Fetch\n298 KB / 0.38s", fill="blue2", line="blue", radius=True, size=14, bold=True)
    add_multiline(s, 6.85, 2.75, 5.25, 1.55, ["在线阶段：每个用户下载 secret-shared recommendation scores", "服务器吞吐：logging >270 q/s；fetch >110 q/s", "well-formedness check：Netflix 上 7.7 min"], size=13, bullet=True)
    add_multiline(s, 0.85, 5.35, 11.7, 0.75, ["扩展：random projection 降维、差分隐私保护模型输出、输入合法性检查防止恶意用户刷影响力。"], size=14, bullet=True)
    add_source(s, "Source: Table 8, Sections 8-9, Nudge")
    add_note(s, "这页把训练和服务分开讲：训练是周期性重成本；在线服务延迟和通信是用户体感。扩展部分只讲思想，不展开公式。")

    # 14 Limitations
    s = new_slide(prs, "局限性很明确：三服务器、半诚实、不覆盖私密取内容", "Limitations and boundaries")
    rect(s, 0.8, 1.35, 3.35, 1.2, "需要 3 台服务器\n且最多攻陷 1 台", fill="amber2", line="amber", radius=True, size=15, bold=True)
    rect(s, 4.95, 1.35, 3.35, 1.2, "只证明半诚实\n不防恶意偏离", fill="red2", line="red", radius=True, size=15, bold=True)
    rect(s, 9.1, 1.35, 3.35, 1.2, "推荐模型本身\n仍可能泄露聚合信息", fill="light", line="line", radius=True, size=15, bold=True)
    add_multiline(s, 1.0, 3.4, 5.25, 1.55, ["用户私密获取 item 不是本文目标", "需要结合 PIR、Tor、Private Relay 等系统", "服务器仍知道用户何时发送消息"], size=14, bullet=True)
    add_multiline(s, 7.0, 3.4, 5.2, 1.55, ["开放问题：恶意安全", "开放问题：更多方且容忍多方被攻陷", "开放问题：2-server 且不损害性能"], size=14, bullet=True)
    add_takeaway(s, "Nudge 解决的是“私密训练与私密返回推荐”，不是完整匿名内容平台。", "red")
    add_source(s, "Source: Limitations, Ethical Considerations, Conclusion, Nudge")
    add_note(s, "这页保持中立，不要把 limitations 弱化。它的价值在强隐私和规模之间找到工程可行点，但前提条件很清楚。")

    # 15 Summary
    s = new_slide(prs, "三句话总结：Nudge 的贡献是算法—密码协议协同设计", "Take-home messages")
    add_text(s, 0.9, 1.35, 11.4, 0.5, "1. 隐私目标清楚：服务器只看到聚合模型，不看到单个用户偏好。", size=19, bold=True)
    add_text(s, 0.9, 2.55, 11.4, 0.5, "2. 技术路线聪明：power iteration 天然适合“矩阵乘 + 简单非线性”的 3PC。", size=19, bold=True, color="blue")
    add_text(s, 0.9, 3.75, 11.4, 0.5, "3. 实验结果扎实：Netflix 50 min / 38.9 GB，质量接近明文矩阵分解。", size=19, bold=True, color="green")
    rect(s, 1.0, 5.35, 11.1, 0.9, "适合汇报时的落点：Nudge 不是最便宜的推荐系统，而是少数能把密码学隐私推到真实推荐规模的系统。", fill="blue2", line="blue", radius=True, size=15, bold=True)
    add_note(s, "收束时提醒：Nudge 的意义在于展示了 numerical linear algebra 与 MPC 协同设计的潜力，可迁移到 PCA、PageRank、spectral clustering 等任务。")

    prs.save(OUT)


def audit():
    prs = Presentation(str(OUT))
    slide_w, slide_h = prs.slide_width, prs.slide_height
    out_of_bounds = []
    text_heavy = []
    notes_count = 0
    for i, slide in enumerate(prs.slides, start=1):
        try:
            if slide.notes_slide and slide.notes_slide.notes_text_frame.text.strip():
                notes_count += 1
        except Exception:
            pass
        char_count = 0
        textbox_count = 0
        for shape in slide.shapes:
            if shape.left < 0 or shape.top < 0 or shape.left + shape.width > slide_w or shape.top + shape.height > slide_h:
                out_of_bounds.append((i, shape.name))
            if getattr(shape, "has_text_frame", False):
                txt = shape.text_frame.text or ""
                if txt.strip():
                    textbox_count += 1
                    char_count += len(txt)
        if char_count > 520 or textbox_count > 12:
            text_heavy.append((i, char_count, textbox_count))
    with ZipFile(OUT) as z:
        media = [n for n in z.namelist() if n.startswith("ppt/media/")]
    return {
        "slides": len(prs.slides),
        "media": len(media),
        "notes": notes_count,
        "out_of_bounds": out_of_bounds,
        "text_heavy": text_heavy,
    }


def write_qa(report):
    QA.write_text(
        "\n".join(
            [
                "# QA report — Nudge paper2ppt",
                "",
                f"- PPTX: `{OUT}`",
                f"- Slide count: {report['slides']}",
                f"- Embedded media files: {report['media']}",
                f"- Slides with speaker notes: {report['notes']}",
                "- Figure assets inserted: Fig.1, Fig.2, Table 3, Fig.4, Fig.5, Table 6, Fig.7, Table 8.",
                "- Self-review: no high-severity unsupported quantitative claims; dense source visuals are either given large slide area or paired with concise interpretation.",
                f"- Shape bounds check: {'pass' if not report['out_of_bounds'] else report['out_of_bounds']}",
                f"- Text-density check: {'pass' if not report['text_heavy'] else report['text_heavy']}",
                "- Design-rhythm check: varied cover, conceptual, workflow, figure-dominant, comparison, and discussion slides; avoided repeated card-only template.",
                "- Rendered preview: not run; no reliable headless PPT renderer was used. Verification used python-pptx reopen, media count, shape bounds, contact sheet inspection, and text-density audit.",
                "- Known limitation: figures are cropped from the PDF rather than original vector source files, so very small table text may still be easier to read in slideshow/fullscreen mode.",
                "",
            ]
        ),
        encoding="utf-8",
    )


if __name__ == "__main__":
    build()
    report = audit()
    write_qa(report)
    print(report)
    print(OUT)
    print(QA)
