# 生成 tq.ico：透明底 + 蓝色四芒星（十字星）徽标
# 只有一个大星体，几何简单、留白充足，缩到 16px 也能看出四个尖
import math
import sys
from PIL import Image, ImageDraw

ICO = r"e:\c++code_learn\TQ\tq.ico"
PREV = r"e:\c++code_learn\TQ\_prev.png"

BLUE = (58, 150, 255)    # 星体：亮蓝，深色/浅色背景上都清晰
CORE = (188, 224, 255)   # 大尺寸时的一点星芯

RATIO = 0.47             # 星芒外接半径 / 画布边长
INNER = 0.235            # 内凹半径比例：越小星芒越细长
TILT = 0.0               # 星芒朝向（度），0 = 正十字


def render(size):
    ss = 8                                   # 超采样倍数，边缘更干净
    s = size * ss
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = cy = s / 2.0
    R = s * RATIO
    r = R * INNER

    poly = []
    for i in range(8):
        a = math.radians(-90.0 + TILT + i * 45.0)
        rad = R if i % 2 == 0 else r               # 四个尖 + 四个内凹，组成四芒星
        poly.append((cx + rad * math.cos(a), cy + rad * math.sin(a)))
    d.polygon(poly, fill=BLUE + (255,))

    if size >= 64:                                 # 大尺寸才加星芯，小了会糊
        rc = R * 0.17
        d.ellipse([cx - rc, cy - rc, cx + rc, cy + rc], fill=CORE + (255,))

    return img.resize((size, size), Image.LANCZOS)


if __name__ == "__main__":
    # 28px 帧给自绘标题栏 1:1 用，不缩放才够清晰
    sizes = [256, 128, 64, 48, 32, 28, 24, 16]
    frames = {sz: render(sz) for sz in sizes}

    if "--preview" in sys.argv:
        bg = Image.new("RGBA", (560, 150), (18, 22, 32, 255))
        bg.alpha_composite(frames[128], (8, 11))
        white = Image.new("RGBA", (110, 110), (255, 255, 255, 255))
        white.alpha_composite(frames[96] if 96 in frames else render(96), (7, 7))
        bg.alpha_composite(white, (150, 20))
        x = 280
        for sz in (48, 32, 28, 24, 16):
            bg.alpha_composite(frames[sz], (x, 75 - sz // 2))
            x += sz + 12
        bg.resize((1120, 300), Image.NEAREST).save(PREV, format="PNG")

    big = frames[256]
    big.save(ICO, format="ICO", sizes=[(sz, sz) for sz in sizes],
             append_images=[frames[sz] for sz in sizes[1:]])
    print("ok")
