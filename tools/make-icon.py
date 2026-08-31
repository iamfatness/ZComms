# Renders src/app/zcomms.ico -- the app icon, in the panel's own
# broadcast-console language: a dark rounded tile (panel #15181D on edge
# #262B32) carrying the XLR-face mark -- ivory ring, three amber pins --
# the same mark as the panel header's .mark badge (owner pick 2026-08-31
# over a dot-and-arcs signal mark: one identity everywhere, and the XLR
# face says intercom). Pin positions mirror ui_html.h's .mark geometry.
# Procedural so the icon is reproducible and tweakable in code review.
#
#   python tools/make-icon.py        (needs Pillow)
from PIL import Image, ImageDraw
import os

RACK = (14, 16, 19, 255)       # #0E1013
PANEL = (21, 24, 29, 255)      # #15181D
EDGE = (58, 64, 72, 255)       # #3A4048 -- brighter than scribe: reads at 16px
AMBER = (232, 163, 61, 255)    # #E8A33D
IVORY = (230, 229, 224, 255)   # the panel's --ivory

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

    # XLR face: ivory connector ring, three amber pins at the panel .mark's
    # positions (two up, one down).
    cx, cy = s * 0.5, s * 0.5
    ring = s * 0.32
    w = max(1, int(s * 0.045))
    d.ellipse([cx - ring, cy - ring, cx + ring, cy + ring],
              outline=IVORY, width=w)
    r_pin = s * 0.085
    for px, py in ((0.298, 0.363), (0.702, 0.363), (0.5, 0.687)):
        x = cx + (px - 0.5) * 2 * ring * 1.02
        y = cy + (py - 0.5) * 2 * ring * 1.02
        d.ellipse([x - r_pin, y - r_pin, x + r_pin, y + r_pin], fill=AMBER)

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
