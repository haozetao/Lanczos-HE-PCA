import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageEnhance

import build_nudge_slides as src


ROOT = Path("/Users/bytedance/PCA")
WORK = ROOT / "workspace" / "nudge-ppt"
OUT = WORK / "outputs-hust-template"
MEDIA = WORK / "template-media" / "2026-5-9"

W, H = src.W, src.H

BLUE = (0, 32, 96)
RED = (192, 0, 0)
LIGHT_BLUE = (15, 83, 140)
PALE = (248, 249, 252)
SOFT_GRAY = (238, 241, 246)
LINE = (210, 216, 226)
INK = (28, 32, 40)
MUTED = (90, 98, 112)


def load_asset(name):
    return Image.open(MEDIA / name).convert("RGBA")


HEADER = load_asset("image1.png")
FOOTER = load_asset("image3.png")
SEAL = Image.open(MEDIA / "image7.jpeg").convert("RGB")
CAMPUS = Image.open(MEDIA / "image6.jpeg").convert("RGB")


def paste_fit(dst, src_img, box, mode="contain"):
    x1, y1, x2, y2 = box
    tw, th = x2 - x1, y2 - y1
    src_rgb = src_img.convert("RGBA") if src_img.mode != "RGBA" else src_img
    sw, sh = src_rgb.size
    scale = max(tw / sw, th / sh) if mode == "cover" else min(tw / sw, th / sh)
    im = src_rgb.resize((int(sw * scale), int(sh * scale)), Image.Resampling.LANCZOS)
    if mode == "cover":
        left = max(0, (im.width - tw) // 2)
        top = max(0, (im.height - th) // 2)
        im = im.crop((left, top, left + tw, top + th))
        dst.paste(im.convert("RGB"), (x1, y1), im if im.mode == "RGBA" else None)
    else:
        px = x1 + (tw - im.width) // 2
        py = y1 + (th - im.height) // 2
        dst.paste(im.convert("RGB"), (px, py), im)


def draw_template_chrome(img, draw, title=None, kicker=None, page_no=None):
    # Header strip from the reference slide deck.
    paste_fit(img, HEADER, (0, 0, W, 106), mode="cover")
    draw.rectangle((0, 0, 18, 106), fill=BLUE)
    paste_fit(img, SEAL, (1510, 14, 1582, 86), mode="cover")
    draw.text((1598, 23), "华中科技大学", font=src.F_SMALL_B, fill=INK)
    draw.text((1598, 52), "网络空间安全学院", font=src.F_TINY, fill=MUTED)
    draw.line((70, 128, 1850, 128), fill=(220, 226, 236), width=2)
    if kicker:
        draw.text((88, 148), kicker, font=src.F_TINY, fill=RED)
    if title:
        draw.text((88, 178), title, font=src.F_TITLE2, fill=BLUE)
    # Bottom accent, approximating the template footer strip.
    draw.line((80, 1010, 1840, 1010), fill=(225, 229, 238), width=2)
    if page_no is not None:
        draw.text((1755, 1024), f"{page_no:02d}", font=src.F_TINY, fill=MUTED)
    draw.text((88, 1024), "Nudge: A Private Recommendations Engine", font=src.F_TINY, fill=MUTED)


def template_base(title=None, kicker=None, dark=False):
    img = Image.new("RGB", (W, H), (255, 255, 255))
    draw = ImageDraw.Draw(img)
    draw_template_chrome(img, draw, title, kicker)
    return img, draw, INK, MUTED


def title_slide():
    img = Image.new("RGB", (W, H), (255, 255, 255))
    d = ImageDraw.Draw(img)
    draw_template_chrome(img, d)
    # Campus image band, borrowing the monthly-meeting visual cue.
    campus = ImageEnhance.Brightness(CAMPUS).enhance(1.08)
    paste_fit(img, campus, (0, 112, W, H), mode="cover")
    overlay = Image.new("RGBA", (W, H), (255, 255, 255, 112))
    img = Image.alpha_composite(img.convert("RGBA"), overlay).convert("RGB")
    d = ImageDraw.Draw(img)
    # Repaint header above overlay.
    draw_template_chrome(img, d)
    d.text((185, 255), "Nudge", font=src.font(116, "bold"), fill=RED)
    d.text((185, 390), "A Private Recommendations Engine", font=src.F_TITLE2, fill=BLUE)
    d.line((185, 475, 1120, 475), fill=BLUE, width=5)
    d.text((185, 535), "Alexandra Henzinger · Emma Dauterman · Henry Corrigan-Gibbs · Dan Boneh", font=src.F_H3, fill=INK)
    d.text((185, 598), "论文汇报：三方 MPC 下的大规模私有推荐系统", font=src.F_H3, fill=BLUE)
    src.rounded(d, (1235, 260, 1715, 665), (255, 255, 255), outline=(210, 216, 226), radius=22)
    x = 1290
    y = 315
    for label in ["Secret-shared ratings", "3 non-colluding servers", "Private matrix factorization", "Personalized recommendations"]:
        src.rounded(d, (x, y, 1665, y + 58), (244, 247, 251), outline=LINE, radius=14)
        d.text((x + 24, y + 17), label, font=src.F_SMALL_B, fill=BLUE)
        y += 82
    d.text((185, 910), "关键词：Recommendation · 3PC · Function Secret Sharing · Power Iteration", font=src.F_SMALL_B, fill=RED)
    return img


def restyle_globals():
    src.BG = (255, 255, 255)
    src.INK = INK
    src.MUTED = MUTED
    src.LINE = LINE
    src.ACCENT = BLUE
    src.ACCENT2 = RED
    src.GREEN = (43, 120, 72)
    src.YELLOW = (210, 151, 40)
    src.DARK = (20, 28, 42)
    src.WHITE = (255, 255, 255)
    src.base = template_base


PLAN = {
    "title": "Nudge: A Private Recommendations Engine",
    "style": "HUST academic report template",
    "style_guidelines": {
        "color_palette": "White background, deep blue #002060 primary text, red #C00000 emphasis, light gray header/footer strips",
        "typography": "Microsoft YaHei-like Chinese/English hierarchy, bold deep-blue titles, compact academic body text",
        "imagery": "Reference-deck header/logo/campus image, paper figure crops, redrawn algorithm diagrams and experiment summaries",
        "layout": "Academic reading-group / monthly-meeting style: title at upper-left, logo at upper-right, restrained charts and figure crops",
    },
    "aspect_ratio": "16:9",
    "slides": [
        {"slide_number": i + 1, "type": "content", "title": title}
        for i, title in enumerate(
            [
                "Nudge: A Private Recommendations Engine",
                "为什么需要 Nudge？",
                "系统总览：三服务器拆分信任",
                "威胁模型：强隐私，但不是万能",
                "矩阵分解直觉：把评分矩阵压成低秩表示",
                "算法步骤：Power Iteration 做私有矩阵分解",
                "为什么这个算法适合 3PC？",
                "复杂度叙事：通信从跟评分数走变成跟用户+物品走",
                "推荐质量：接近明文矩阵分解",
                "性能：私有矩阵分解从天/小时压到秒/分钟",
                "端到端与横向扩展：训练重，服务轻",
                "局限、扩展与汇报结论",
            ]
        )
    ],
}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    restyle_globals()
    slides = [
        title_slide,
        src.slide2,
        src.slide3,
        src.slide4,
        src.slide5,
        src.slide6,
        src.slide7,
        src.slide8,
        src.slide9,
        src.slide10,
        src.slide11,
        src.slide12,
    ]
    with open(WORK / "nudge-plan-hust-template.json", "w", encoding="utf-8") as f:
        json.dump(PLAN, f, ensure_ascii=False, indent=2)
    for idx, fn in enumerate(slides, start=1):
        img = fn()
        d = ImageDraw.Draw(img)
        if idx > 1:
            # Page number is added after slide content to keep it visible.
            d.text((1755, 1024), f"{idx:02d}", font=src.F_TINY, fill=MUTED)
        path = OUT / f"nudge-hust-slide-{idx:02d}.jpg"
        img.save(path, quality=95)
        print(path)


if __name__ == "__main__":
    main()
