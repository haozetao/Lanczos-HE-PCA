import json
from pathlib import Path
from collections import Counter


WORK = Path("/Users/bytedance/PCA/workspace/nudge-ppt")
RAW_IN = WORK / "current_whiteboard_raw.json"
RAW_OUT = WORK / "nudge_academic_whiteboard_raw.json"


def dominant_parent():
    if RAW_IN.exists():
        data = json.loads(RAW_IN.read_text())
        parents = Counter(n.get("parent_id") for n in data.get("nodes", []) if n.get("parent_id"))
        if parents:
            return parents.most_common(1)[0][0]
    return "t2:2"


PARENT = dominant_parent()
nodes = []
z = 1
id_map = {}
shape_seq = 1
conn_seq = 1


def style(fill="#ffffff", border="#d5dbe7", width="narrow", opacity=100):
    return {
        "border_color": border,
        "border_color_type": 1,
        "border_opacity": 100,
        "border_style": "solid",
        "border_width": width,
        "fill_color": fill,
        "fill_color_type": 1,
        "fill_opacity": opacity,
        "h_flip": False,
        "v_flip": False,
    }


def text_style(text, size=16, color="#1f2329", weight="regular", align="center", valign="mid"):
    return {
        "angle": 0,
        "font_size": size,
        "font_weight": weight,
        "horizontal_align": align,
        "italic": False,
        "line_through": False,
        "text": text,
        "text_background_color_type": 0,
        "text_color": color,
        "text_color_type": 0,
        "theme_text_background_color_code": -1,
        "theme_text_color_code": -1,
        "underline": False,
        "vertical_align": valign,
    }


def add_shape(id_, x, y, w, h, text="", fill="#ffffff", border="#d5dbe7", size=16, color="#1f2329",
              weight="regular", align="center", radius=True, border_width="narrow", opacity=100):
    global z, shape_seq
    api_id = f"o3:{shape_seq}"
    shape_seq += 1
    id_map[id_] = api_id
    node = {
        "angle": 0,
        "composite_shape": {"type": "round_rect" if radius else "rect"},
        "height": h,
        "id": api_id,
        "parent_id": PARENT,
        "style": style(fill, border, border_width, opacity),
        "type": "composite_shape",
        "width": w,
        "x": x,
        "y": y,
        "z_index": z,
    }
    if text:
        node["text"] = text_style(text, size=size, color=color, weight=weight, align=align)
    nodes.append(node)
    z += 1
    return id_


def add_conn(id_, start_id, end_id, start_snap="right", end_snap="left", color="#334155", width="narrow"):
    global z, conn_seq
    api_id = f"c3:{conn_seq}"
    conn_seq += 1
    pos = {
        "left": {"x": 0, "y": 0.5},
        "right": {"x": 1, "y": 0.5},
        "top": {"x": 0.5, "y": 0},
        "bottom": {"x": 0.5, "y": 1},
    }
    node = {
        "angle": 0,
        "connector": {
            "caption_auto_direction": False,
            "end": {
                "arrow_style": "triangle_arrow",
                "attached_object": {"id": id_map[end_id], "position": pos[end_snap], "snap_to": end_snap},
            },
            "end_object": {"id": id_map[end_id], "position": pos[end_snap], "snap_to": end_snap},
            "shape": "straight",
            "specified_coordinate": True,
            "start": {
                "arrow_style": "none",
                "attached_object": {"id": id_map[start_id], "position": pos[start_snap], "snap_to": start_snap},
            },
            "start_object": {"id": id_map[start_id], "position": pos[start_snap], "snap_to": start_snap},
            "turning_points": [],
        },
        "height": 0,
        "id": api_id,
        "parent_id": PARENT,
        "style": {
            "border_color": color,
            "border_color_type": 1,
            "border_opacity": 100,
            "border_style": "solid",
            "border_width": width,
            "theme_border_color_code": -1,
        },
        "type": "connector",
        "width": 0,
        "x": 0,
        "y": 0,
        "z_index": z,
    }
    nodes.append(node)
    z += 1


# Canvas background and title
add_shape("nudge_bg", -40, -40, 1880, 1170, "", fill="#fbfcff", border="#fbfcff", radius=False, opacity=100)
add_shape(
    "nudge_title",
    40,
    30,
    1260,
    86,
    "Nudge: A Private Recommendations Engine\nThree-party private matrix factorization protocol",
    fill="#ffffff",
    border="#ffffff",
    size=28,
    color="#0f172a",
    weight="bold",
    align="left",
    radius=False,
)
add_shape("legend_h", 1330, 42, 135, 34, "hidden", fill="#edf4ff", border="#3b82f6", size=14, color="#0f2f68")
add_shape("legend_r", 1480, 42, 150, 34, "revealed", fill="#fff1f1", border="#c00000", size=14, color="#8a1111")
add_shape("legend_l", 1645, 42, 145, 34, "local / clear", fill="#eefbf1", border="#2e7d50", size=14, color="#166534")

# Protocol columns
cols = [
    ("lane_user", 60, "Users"),
    ("lane_client", 330, "Client-side input"),
    ("lane_servers", 640, "Three Nudge servers"),
    ("lane_model", 1030, "Learned model / outputs"),
    ("lane_privacy", 1370, "Privacy boundary"),
]
for lane_id, x, label in cols:
    add_shape(lane_id, x, 145, 240 if lane_id != "lane_servers" else 330, 820, "", fill="#ffffff", border="#e4e7ed", radius=True)
    add_shape(lane_id + "_head", x + 12, 158, (216 if lane_id != "lane_servers" else 306), 42, label, fill="#f1f5fb", border="#d5dbe7", size=17, color="#0f2f68", weight="bold")

# Phase labels
phases = [
    (218, "0. Setup & input"),
    (360, "1. Private collection"),
    (520, "2. Private factorization"),
    (710, "3. Serving"),
    (850, "4. What is revealed"),
]
for y, label in phases:
    add_shape(f"phase_{y}", 40, y, 1750, 28, label, fill="#f8fafc", border="#e2e8f0", size=14, color="#64748b", weight="bold", align="left", radius=False)

# Setup & input
add_shape("u_rating", 85, 255, 190, 78, "rating vector u(i)\nitems user interacted with", fill="#edf4ff", border="#3b82f6", size=15, color="#0f172a")
add_shape("matrix_u", 365, 255, 200, 78, "U ∈ R^{m×n}\nuser-item matrix", fill="#edf4ff", border="#3b82f6", size=17, color="#0f172a", weight="bold")
add_shape("params", 682, 248, 285, 92, "public parameters\nembedding d, iterations ℓ\nring Z₂ᵇ, fractional bits t", fill="#ffffff", border="#94a3b8", size=15, color="#1f2937")

# Collection
add_shape("dpf_share", 365, 395, 205, 86, "succinct secret sharing\nvia distributed point functions", fill="#eefbf1", border="#2e7d50", size=15, color="#14532d")
add_shape("servers_hold", 680, 392, 295, 92, "servers hold replicated shares ⟦U⟧\nno server sees rating locations or values", fill="#111827", border="#111827", size=15, color="#ffffff", weight="bold")
add_shape("wellformed", 690, 495, 275, 66, "one-time well-formedness check\nratings ∈ {0,1}", fill="#fff7ed", border="#f59e0b", size=14, color="#7c2d12")

# Factorization
add_shape("program", 675, 585, 305, 100, "Matrix-vector program\nalternate: matrix-vector multiply\nand nonlinear vector functions", fill="#111827", border="#111827", size=16, color="#ffffff", weight="bold")
add_shape("linear", 695, 705, 125, 78, "linear layers\nU·v, Uᵀ·v\nnon-interactive", fill="#edf4ff", border="#3b82f6", size=14, color="#0f172a")
add_shape("nonlinear", 845, 705, 125, 78, "nonlinear layers\nTruncₜ\nNormalize", fill="#fff1f1", border="#c00000", size=14, color="#0f172a")
add_shape("power_iter", 1045, 575, 235, 112, "Power iteration\nv ← Uᵀ(Uv)\nSetOrthogonal(v,B)\nNormalize(v)", fill="#eefbf1", border="#2e7d50", size=15, color="#14532d")
add_shape("model_b", 1050, 712, 225, 82, "B ∈ R^{d×n}\nitem embeddings\nrevealed model", fill="#fff1f1", border="#c00000", size=16, color="#8a1111", weight="bold")

# Serving
add_shape("new_user", 92, 742, 175, 70, "user i\nnew or existing", fill="#edf4ff", border="#3b82f6", size=15, color="#0f172a")
add_shape("score", 1040, 820, 245, 82, "recommendation scores\nu(i) · BᵀB\nreturned as shares", fill="#ffffff", border="#64748b", size=15, color="#1f2937")
add_shape("topk", 1390, 820, 210, 82, "client reconstructs\ntop-k recommendations", fill="#edf4ff", border="#3b82f6", size=15, color="#0f172a")

# Privacy boundary
add_shape("hidden_box", 1390, 250, 330, 145, "Hidden from every single server\n• which items each user rated\n• rating values and positions\n• intermediate private vectors", fill="#edf4ff", border="#3b82f6", size=16, color="#0f172a", align="left")
add_shape("leak_box", 1390, 430, 330, 145, "Allowed leakage\n• final aggregate model B\n• number of ratings per user\n• submission timing metadata", fill="#fff1f1", border="#c00000", size=16, color="#0f172a", align="left")
add_shape("limits_box", 1390, 610, 330, 135, "Security limits\n• semi-honest only\n• not secure if ≥2 servers collude\n• model may encode aggregate behavior", fill="#fff7ed", border="#f59e0b", size=16, color="#0f172a", align="left")

# Evaluation footnote row
add_shape("eval", 55, 1000, 1720, 82, "Evaluation anchor: Netflix 463K users / 17K items / 56M ratings  ·  private training 50 min over LAN  ·  40 GB server-to-server communication  ·  nDCG@20 = 0.29", fill="#ffffff", border="#cbd5e1", size=18, color="#0f2f68", weight="bold")

# Connectors
add_conn("c1", "u_rating", "matrix_u")
add_conn("c2", "matrix_u", "dpf_share")
add_conn("c3", "dpf_share", "servers_hold")
add_conn("c4", "servers_hold", "program", "bottom", "top")
add_conn("c5", "program", "linear", "bottom", "top")
add_conn("c6", "linear", "nonlinear")
add_conn("c7", "nonlinear", "power_iter")
add_conn("c8", "power_iter", "model_b", "bottom", "top")
add_conn("c9", "model_b", "score", "bottom", "top")
add_conn("c10", "new_user", "score")
add_conn("c11", "score", "topk")
add_conn("c12", "servers_hold", "hidden_box")
add_conn("c13", "model_b", "leak_box")
add_conn("c14", "servers_hold", "wellformed", "bottom", "top")

RAW_OUT.write_text(json.dumps({"nodes": nodes}, ensure_ascii=False, indent=2), encoding="utf-8")
print(RAW_OUT)
print(f"nodes={len(nodes)} parent={PARENT}")
