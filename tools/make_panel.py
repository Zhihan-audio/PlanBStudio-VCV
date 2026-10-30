#!/usr/bin/env python3
"""生成 Splinter 的面板 SVG 和旋钮 SVG。

运行：python3 tools/make_panel.py
布局常量必须和 src/Splinter.cpp 顶部的保持一致。
Rack 的 SVG 渲染器不支持 <text>，所以面板文字在 C++ 里用 nanovg 画；这里只画形状和 Plan B logo。
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "res")
# Plan B logo 套件放在项目旁边；也可以用环境变量 PLANB_LOGO_SVG 指定。
# 编译插件不需要它，只有重新生成面板时才用到。
LOGO_SVG = os.environ.get("PLANB_LOGO_SVG", os.path.join(
    ROOT, "..", "Plan-B-Studio-Logo", "final-package-unbounded", "svg", "planb-horizontal-paper.svg"))

# 黑 / 粉 / 白
BLACK = "#0D0B0E"      # 带一点莓紫的黑
LEFT_BG = "#141116"
HAIR = "#2A2229"
PINK = "#FF5C9E"       # 主色：热粉
PINK_SOFT = "#FFB1CF"  # 辅色：浅粉
WHITE = "#FFF6FA"      # 暖白
PAPER = "#F2F3F1"      # Plan B logo 官方纸白

W, H = 325.12, 128.5   # 64HP
LEFT_W = 56.0
LEFT_X = [10.0, 22.0, 34.0, 46.0]
LEFT_Y = [34.0, 48.0, 62.0, 76.0, 90.0, 104.0, 118.0]


def step_x(i):
    return 72.5 + i * 7.5 + (i // 4) * 1.6


def rr(x, y, w, h, r, fill, extra=""):
    return f'<rect x="{x:.2f}" y="{y:.2f}" width="{w:.2f}" height="{h:.2f}" rx="{r}" fill="{fill}" {extra}/>'


def logo_group(x_right, y_top, height):
    """把 logo 横版的路径嵌进面板（官方纸白版）。"""
    src = open(LOGO_SVG, encoding="utf-8").read()
    stroke_body = re.search(r'<g fill="none" stroke="currentColor"[^>]*>(.*?)</g>', src, re.S).group(1)
    fill_body = re.search(r'<g fill="currentColor">(.*?)</g>', src, re.S).group(1)
    # 内容边界（含 1.6 的描边半宽）
    bx0, by0, bx1, by1 = 11.4, 26.4, 240.4, 73.6
    s = height / (by1 - by0)
    tx = x_right - (bx1 - bx0) * s - bx0 * s
    ty = y_top - by0 * s
    return (f'<g transform="translate({tx:.3f} {ty:.3f}) scale({s:.5f})">'
            f'<g fill="none" stroke="{PAPER}" stroke-width="3.2" stroke-linecap="round" stroke-linejoin="round">{stroke_body}</g>'
            f'<g fill="{PAPER}">{fill_body}</g></g>')


def broken_heart(x, y, size):
    """标题前的小碎心：沿锯齿裂缝分成两半，以底尖为轴向两边微微张开。"""
    k = size / 10.0
    crack = "L4.1 3.7 L5.7 5.2 L4.7 7.9 L5 9"
    left = f"M5 1.2 {crack} C2 7 0 5 0 3 C0 1.3 1.3 0 3 0 C4 0 4.6 0.6 5 1.2 Z"
    right = "M5 1.2 C5.4 0.6 6 0 7 0 C8.7 0 10 1.3 10 3 C10 5 8 7 5 9 L4.7 7.9 L5.7 5.2 L4.1 3.7 Z"
    return (f'<g transform="translate({x:.3f} {y:.3f}) scale({k:.4f})">'
            f'<path d="{left}" fill="{PINK}" transform="translate(-0.25 0) translate(5 9) rotate(-7) translate(-5 -9)"/>'
            f'<path d="{right}" fill="{PINK}" transform="translate(0.45 0) translate(5 9) rotate(9) translate(-5 -9)"/>'
            f'</g>')


def panel():
    p = []
    p.append(rr(0, 0, W, H, 0, BLACK))
    # 左侧全局区
    p.append(rr(0, 0, LEFT_W, H, 0, LEFT_BG))
    p.append(rr(LEFT_W, 0, 0.3, H, 0, HAIR))
    # 显示屏
    p.append(rr(4, 6.5, 48, 18, 2.2, "#08070A", f'stroke="{HAIR}" stroke-width="0.3"'))
    p.append(rr(59, 12.5, 261, 25.5, 2.2, "#08070A", f'stroke="{HAIR}" stroke-width="0.3"'))
    # 采样卡片（SLICES / TRANS / BITS / RATE）
    p.append(rr(3.5, 39.6, 49, 14.8, 2.0, PINK, 'fill-opacity="0.07"'))
    # 打击乐卡片（L 形：ATK..REL + PUNCH/DRIVE）
    x0, x1, xm, y0, ym, y1, r = 3.5, 52.5, 28.0, 54.8, 68.6, 82.6, 2.0
    p.append(f'<path d="M{x0+r} {y0} H{x1-r} Q{x1} {y0} {x1} {y0+r} V{ym-r} Q{x1} {ym} {x1-r} {ym} '
             f'H{xm+r} Q{xm} {ym} {xm} {ym+r} V{y1-r} Q{xm} {y1} {xm-r} {y1} H{x0+r} Q{x0} {y1} {x0} {y1-r} '
             f'V{y0+r} Q{x0} {y0} {x0+r} {y0} Z" fill="{WHITE}" fill-opacity="0.045"/>')
    # 输出卡片
    p.append(rr(16.5, 110.4, 36, 12.4, 2.0, "#221C24"))

    # 32 步：每 4 步一组的底板
    for g in range(8):
        xa = step_x(4 * g) - 3.75
        xb = step_x(4 * g + 3) + 3.75
        fill = "#1A161C" if g % 2 == 0 else "#151217"
        p.append(rr(xa, 40.0, xb - xa, 77.5, 2.0, fill))
    # 第 16 / 17 步之间的分隔
    mid = (step_x(15) + step_x(16)) / 2
    p.append(rr(mid - 0.15, 41.5, 0.3, 74.5, 0.15, PINK, 'fill-opacity="0.45"'))
    # PERC 行上方的细线
    p.append(rr(step_x(0) - 3.75, 108.4, step_x(31) - step_x(0) + 7.5, 0.25, 0, WHITE, 'fill-opacity="0.18"'))

    # 标题前的小碎心
    p.append(broken_heart(59.4, 4.0, 4.4))
    # 顶栏 logo（右上角螺丝左边）
    p.append(logo_group(x_right=311.5, y_top=2.0, height=7.4))

    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" height="{H}mm" viewBox="0 0 {W} {H}">\n  '
            + "\n  ".join(p) + "\n</svg>\n")


def knob(name, box, body, dot_r, dot_size, dot_color=WHITE, light=False):
    c = box / 2
    if light:
        top, bottom, cap, rim = "#FFFFFF", "#D9CBD3", "#F6EDF2", "#9E8C97"
    else:
        top, bottom, cap, rim = "#3A3139", "#141015", "#1E1920", "#050405"
    bg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{box}mm" height="{box}mm" viewBox="0 0 {box} {box}">'
          f'<defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1">'
          f'<stop offset="0" stop-color="{top}"/><stop offset="1" stop-color="{bottom}"/></linearGradient></defs>'
          f'<circle cx="{c}" cy="{c}" r="{body}" fill="url(#g)"/>'
          f'<circle cx="{c}" cy="{c}" r="{body * 0.84:.3f}" fill="{cap}"/>'
          f'<circle cx="{c}" cy="{c}" r="{body}" fill="none" stroke="{rim}" stroke-width="{body * 0.045:.3f}"/>'
          f'</svg>\n')
    fg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{box}mm" height="{box}mm" viewBox="0 0 {box} {box}">'
          f'<circle cx="{c}" cy="{c - dot_r:.3f}" r="{dot_size}" fill="{dot_color}"/></svg>\n')
    open(os.path.join(RES, f"knob-{name}-bg.svg"), "w").write(bg)
    open(os.path.join(RES, f"knob-{name}-fg.svg"), "w").write(fg)


if __name__ == "__main__":
    os.makedirs(RES, exist_ok=True)
    open(os.path.join(RES, "Splinter.svg"), "w", encoding="utf-8").write(panel())
    # 名称, 盒子尺寸, 旋钮半径, 圆点到中心距离, 圆点半径（mm）
    knob("step", 6.8, 2.45, 1.5, 0.4)
    knob("mid", 9.6, 3.5, 2.25, 0.52)
    knob("big", 12.4, 4.7, 3.05, 0.78, PINK, light=True)
    print("panel + knobs written to", RES)
