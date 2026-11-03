#!/usr/bin/env python3
"""Draw the icon's foreground: the Atari 5200 controller, from the console photo.

Tall stick with a round knob on a round base, START and PAUSE bars, a 4x3
keypad, all on the angled body. White = shape (becomes brushed silver in
make-icons.py), black = detail cut-outs (becomes dark steel). Output is the
shared foreground format (432x432 RGBA, alpha = shape); make-icons.py recolours
it. Re-run only when the art changes:

    python3 tools/icons/draw-mark.py && python3 tools/icons/make-icons.py
"""
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "data/icons/src/fujinet-go-atari5200-foreground.png"
S = 1000          # local drawing frame
SS = 3            # supersampling
SIZE = 432
TILT = 14         # degrees, the controller leans left as in the photo

WHITE = (255, 255, 255, 255)
BLACK = (0, 0, 0, 255)


def draw() -> Image.Image:
    n = S * SS
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)

    def rr(x0, y0, x1, y1, r, fill):
        d.rounded_rectangle((x0 * SS, y0 * SS, x1 * SS, y1 * SS), r * SS, fill=fill)

    def el(cx, cy, rx, ry, fill):
        d.ellipse(((cx - rx) * SS, (cy - ry) * SS, (cx + rx) * SS, (cy + ry) * SS), fill=fill)

    # body
    rr(320, 330, 680, 940, 46, WHITE)
    # START / PAUSE
    rr(342, 352, 402, 384, 8, BLACK)
    rr(598, 352, 658, 384, 8, BLACK)
    # round base, with the stick's collar
    el(500, 440, 104, 104, BLACK)
    el(500, 440, 74, 74, WHITE)
    el(500, 440, 40, 40, BLACK)
    # stick and knob
    rr(478, 140, 522, 440, 20, WHITE)
    el(500, 160, 74, 86, WHITE)
    rr(452, 160, 548, 174, 6, BLACK)      # the knob's ridges
    rr(456, 130, 544, 142, 5, BLACK)
    # keypad 4 x 3
    cw, ch, gx, gy = 72, 62, 18, 16
    x0, y0 = 500 - (3 * cw + 2 * gx) / 2, 590
    for r in range(4):
        for c in range(3):
            x = x0 + c * (cw + gx)
            y = y0 + r * (ch + gy)
            rr(x, y, x + cw, y + ch, 14, BLACK)

    im = im.rotate(TILT, resample=Image.BICUBIC, center=(n / 2, n / 2))
    scale = SIZE * 0.84 / (S * SS)   # the drawing spans ~0.87 of the frame, ~0.73 of the canvas
    side = int(n * scale)
    im = im.resize((side, side), Image.LANCZOS)
    out = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    out.paste(im, ((SIZE - side) // 2 + 4, (SIZE - side) // 2 + 4), im)
    return out


if __name__ == "__main__":
    draw().save(OUT, optimize=True)
    print(f"wrote {OUT.relative_to(ROOT)}")
