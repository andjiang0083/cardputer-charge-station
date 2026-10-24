#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make-cover.py — 生成 M5Burner 商店封面（1280×720）

主视觉 = 真实数据：2026-10-24 整夜实测的 44 条 10 分钟记录（3.90V → 4.06V 单调上升，
末段为拔出瞬间的断崖）。曲线不是示意波形，是这台机器实际跑出来的那一条。

规矩来自 skill `m5burner-store-art`：画布 1280×720 / 文字底线 y≤561 / 左上 300×110 留空 /
左右 56px 安全边 / 图内不印版本号 / 中英双语。文字门禁直接调 mkcover.gate_texts（唯一实现）。

注意：电压是小数，而门禁把 `\\d+\\.\\d+` 判成“版本号”——所以图上不写任何小数数字，
用颜色和图例表达，数字留给真机屏幕。

用法: python3 tools/make-cover.py
产物: releases/cover-1280x720-cardputer.png + 同名 .texts.json
"""
import json, os, sys
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(ROOT), "tools"))
import mkcover as mk                                     # 复用唯一实现：字体、门禁、台账

W, H, SAFE = mk.W, mk.H, mk.SAFE
AMBER = (253, 176, 32)
GREEN = (60, 220, 120)
GREY = (150, 150, 158)
WHITE, DIM = mk.WHITE, (138, 138, 146)
BG = (10, 10, 13)

# 2026-10-24 整夜实测（mV，每 10 分钟一条，来自 NVS CST3）
SERIES = [3896, 3899, 3904, 3907, 3912, 3918, 3921, 3922, 3928, 3930, 3934, 3940,
          3946, 3948, 3949, 3954, 3957, 3964, 3968, 3972, 3976, 3978, 3984, 3996,
          3999, 4001, 4005, 4010, 4012, 4020, 4022, 4024, 4029, 4033, 4034, 4043,
          4047, 4048, 4050, 4055, 4056, 4060, 4064]


def chip(d, x, y, color, r=9):
    d.rounded_rectangle([x, y, x + 2 * r, y + 2 * r], radius=4, fill=color)


def main():
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    # 极淡网格（暗场质感，不抢文字）
    for gx in range(0, W, 64):
        d.line([(gx, 0), (gx, H)], fill=(18, 18, 22))
    for gy in range(0, H, 64):
        d.line([(0, gy), (W, gy)], fill=(18, 18, 22))

    # ── 右栏：上=真机实拍（主视觉），下=真实整夜曲线 ────────────────
    RX0, RX1 = 742, 1224
    # 实拍：屏幕特写，按 16:9 覆盖裁切
    ph = Image.open(os.path.join(ROOT, "docs/images/charge-station-cardputer.jpg")).convert("RGB")
    tw, th = RX1 - RX0, 271
    sc = max(tw / ph.width, th / ph.height)
    ph = ph.resize((int(ph.width * sc + 0.5), int(ph.height * sc + 0.5)), mk.RESAMPLE)
    left = (ph.width - tw) // 2; top = (ph.height - th) // 2
    ph = ph.crop((left, top, left + tw, top + th))
    mask = Image.new("L", (tw, th), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, tw - 1, th - 1], radius=16, fill=255)
    img.paste(ph, (RX0, 138), mask)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([RX0, 138, RX1, 138 + th], radius=16, outline=(46, 46, 54), width=2)

    px0, py0, px1, py1 = RX0, 428, RX1, 556
    d.rounded_rectangle([px0, py0, px1, py1], radius=14, fill=(16, 16, 20), outline=(34, 34, 40), width=2)

    vmin, vmax = min(SERIES), max(SERIES)
    span = max(60, vmax - vmin)
    n = len(SERIES)
    pts = []
    for i, mv in enumerate(SERIES):
        x = px0 + 18 + (px1 - px0 - 36) * i / (n - 1)
        y = py1 - 26 - (py1 - py0 - 62) * (mv - vmin) / span
        pts.append((x, y))

    # 辉光：单独一层高斯模糊后叠加
    glow = Image.new("RGB", (W, H), (0, 0, 0))
    ImageDraw.Draw(glow).line(pts, fill=(110, 74, 10), width=7, joint="curve")
    glow = glow.filter(mk.ImageFilter.GaussianBlur(5))
    img = Image.blend(img, glow, 0.28)
    d = ImageDraw.Draw(img)
    d.line(pts, fill=AMBER, width=4, joint="curve")
    d.ellipse([pts[-1][0] - 6, pts[-1][1] - 6, pts[-1][0] + 6, pts[-1][1] + 6], fill=WHITE)
    # 卡片内网格：横向 4 等分 + 每小时一根短刻度（只画线，不标数字）
    for k in range(1, 4):
        gy = py0 + (py1 - py0) * k / 4
        d.line([(px0 + 14, gy), (px1 - 14, gy)], fill=(30, 30, 36), width=1)
    for i in range(0, n, 6):
        x = px0 + 18 + (px1 - px0 - 36) * i / (n - 1)
        d.line([(x, py1 - 26), (x, py1 - 32)], fill=(58, 58, 66), width=1)
    # 拔出瞬间：虚线标记（真实数据末段的断崖）
    for yy in range(int(pts[-1][1]), py1 - 26, 8):
        d.line([(pts[-1][0], yy), (pts[-1][0], yy + 4)], fill=GREY, width=2)

    # ── 文字统一在最后的画布上画，保证每一条都进台账（门禁要查）────
    c = mk.Canvas(img)
    d = c.d
    c.text(SAFE, 118, "CHARGE STATION", mk.font(mk.ASCII_BOLD, 62), AMBER, "title")
    c.text(SAFE, 212, "插上电，到底在不在充？", mk.font(mk.CJK, 42), WHITE, "sub")
    c.text(SAFE, 268, "Know if it is really charging — in one color.",
           mk.font(mk.ASCII, 25), DIM, "sub_en")

    for i, (label, color) in enumerate([("正在充 · CHARGING", AMBER),
                                        ("已充满 · FULL", GREEN),
                                        ("没在充 · NO CHARGE", GREY)]):
        y = 330 + i * 52
        chip(d, SAFE, y + 12, color)
        c.text(SAFE + 30, y, label, mk.font(mk.CJK, 27), WHITE, "bullet")

    c.text(SAFE, 496, "灭屏 · 降频 · 不启射频，电流全给电池",
           mk.font(mk.CJK, 24), DIM, "bullet")
    c.text(SAFE, 528, "Dark screen · low clock · no radio", mk.font(mk.ASCII, 22), DIM, "bullet")

    c.text(px0 + 14, py0 + 6, "24 h log", mk.font(mk.ASCII_BOLD, 18), AMBER, "body")
    c.text(px0 + 120, py0 + 6, "整夜实测曲线 · 每 10 分钟一点", mk.font(mk.CJK, 15), DIM, "body")

    out_dir = os.path.join(ROOT, "releases")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "cover-1280x720-cardputer.png")
    c.gate("Charge Station")          # 越界/双语/版本号，机器判
    c.img.save(out)
    meta = {"entry": "Charge Station", "cover": os.path.relpath(out, ROOT),
            "canvas": [W, H], "layout": "data", "hero": {"kind": "photo", "path": "docs/images/charge-station-cardputer.jpg"},
            "texts": c.texts, "generator": "tools/make-cover.py",
            "generated": mk.datetime.now().isoformat(timespec="seconds")}
    json.dump(meta, open(out.replace(".png", ".texts.json"), "w", encoding="utf-8"),
              ensure_ascii=False, indent=1)
    print("封面:", out, "| 文字块:", len(c.texts))


if __name__ == "__main__":
    main()
