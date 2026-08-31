# Renders src/app/zcomms.ico -- the app icon, in the panel's own
# broadcast-console language: a dark rounded tile (panel #15181D on edge
# #262B32) carrying an amber (#E8A33D) transmit mark, a dot with two
# radiating arcs. Procedural so the icon is reproducible and tweakable in
# code review like everything else here.
#
#   python tools/make-icon.py        (needs Pillow)
from PIL import Image, ImageDraw
import os

RACK = (14, 16, 19, 255)       # #0E1013
PANEL = (21, 24, 29, 255)      # #15181D
EDGE = (58, 64, 72, 255)       # #3A4048 -- brighter than scribe: reads at 16px
AMBER = (232, 163, 61, 255)    # #E8A33D

SS = 8  # supersample factor; arcs need it to stay clean at small sizes


def render(size: int) -> Image.Image:
    s = size * SS
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # Tile: rounded square, slight inset so Explorer selection outlines
    # do not clip the corners.
    inset = s * 0.03
    radius = s * 0.22
    d.rounded_rectangle([inset, inset, s - inset, s - inset], radius=radius,
                        fill=PANEL, outline=EDGE, width=max(1, s // 32))

    # Transmit mark, centered optically a touch left so the arcs balance:
    # a solid dot with two arcs radiating to the right.
    cx, cy = s * 0.38, s * 0.50
    r_dot = s * 0.11
    d.ellipse([cx - r_dot, cy - r_dot, cx + r_dot, cy + r_dot], fill=AMBER)
    w = max(1, int(s * 0.075))
    for r in (s * 0.24, s * 0.38):
        d.arc([cx - r, cy - r, cx + r, cy + r], start=-48, end=48,
              fill=AMBER, width=w)

    return img.resize((size, size), Image.LANCZOS)


def main() -> None:
    out = os.path.join(os.path.dirname(__file__), "..", "src", "app",
                       "zcomms.ico")
    sizes = [16, 24, 32, 48, 64, 128, 256]
    imgs = [render(n) for n in sizes]
    imgs[-1].save(out, format="ICO", sizes=[(n, n) for n in sizes],
                  append_images=imgs[:-1])
    print("wrote", os.path.normpath(out), os.path.getsize(out), "bytes")


if __name__ == "__main__":
    main()
