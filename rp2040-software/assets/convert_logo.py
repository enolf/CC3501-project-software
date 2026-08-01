#!/usr/bin/env python3
"""Convert the idle-screen logo from PNG into an LVGL image, ready to compile.

    pip install pillow
    python convert_logo.py logo.png

Writes ../src/assets/idle_logo.c, which CMake compiles when built with
-DIDLE_LOGO=ON.

WHY A SCRIPT RATHER THAN LVGL'S ONLINE CONVERTER
------------------------------------------------
Same result, but this one is in the repository: the conversion settings are
recorded, the output is reproducible by anyone who clones this, and re-running
it after an artwork tweak is one command rather than a series of clicks and a
remembered set of options.

FORMAT
------
RGB565 with no alpha channel, because the idle screen behind it is solid black
and any transparency is flattened onto black here instead. That halves the
memory an ARGB8888 image would need and removes per-pixel blending at draw time,
for a result that is pixel-identical on this screen.

At the recommended 240x140 that is about 67 kB of flash, against roughly 1.5 MB
free. If a smaller asset is ever wanted, a single-colour logo converted to I1
(1 bit per pixel) would be about 4 kB — but there is no reason to bother yet.
"""

import sys
import os

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is not installed.  Run:  pip install pillow")


# The panel is 320x240. Staying inside this leaves a comfortable margin and
# keeps the asset small; anything larger is scaled down rather than refused.
MAX_WIDTH = 260
MAX_HEIGHT = 170

# Matches the idle screen, so flattened transparency is invisible.
BACKGROUND = (0, 0, 0)

OUTPUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "..", "src", "assets", "idle_logo.c")


def to_rgb565(r: int, g: int, b: int) -> int:
    """Pack 8-8-8 into 5-6-5, matching LV_COLOR_DEPTH 16."""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)

    source = sys.argv[1]
    image = Image.open(source)
    print(f"read {source}: {image.width}x{image.height}, mode {image.mode}")

    # Flatten any transparency onto the idle screen's background colour. Done
    # here rather than kept as an alpha channel: the background is known and
    # constant, so blending at draw time would compute the same answer forever.
    if image.mode in ("RGBA", "LA", "P"):
        image = image.convert("RGBA")
        flattened = Image.new("RGB", image.size, BACKGROUND)
        flattened.paste(image, mask=image.split()[-1])
        image = flattened
        print("  flattened transparency onto black")
    else:
        image = image.convert("RGB")

    if image.width > MAX_WIDTH or image.height > MAX_HEIGHT:
        image.thumbnail((MAX_WIDTH, MAX_HEIGHT), Image.LANCZOS)
        print(f"  scaled down to {image.width}x{image.height}")

    width, height = image.size
    pixels = image.load()

    # Little-endian byte order, which is what the RP2040 and the ILI9341 driver
    # in this project both expect.
    data = bytearray()
    for y in range(height):
        for x in range(width):
            value = to_rgb565(*pixels[x, y])
            data.append(value & 0xFF)
            data.append((value >> 8) & 0xFF)

    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as out:
        out.write(f"""// GENERATED FILE - do not edit by hand.
//
// Produced by assets/convert_logo.py from {os.path.basename(source)}
// ({width}x{height}, RGB565, transparency flattened onto black).
// Re-run that script after changing the artwork.
//
// Compiled only when the firmware is built with -DIDLE_LOGO=ON.

#include "lvgl/lvgl.h"

static const uint8_t idle_logo_map[] = {{
""")

        for i in range(0, len(data), 16):
            row = ", ".join(f"0x{b:02X}" for b in data[i:i + 16])
            out.write(f"    {row},\n")

        out.write(f"""}};

const lv_image_dsc_t idle_logo = {{
    .header = {{
        .magic  = LV_IMAGE_HEADER_MAGIC,
        .cf     = LV_COLOR_FORMAT_RGB565,
        .flags  = 0,
        .w      = {width},
        .h      = {height},
        .stride = {width * 2},
    }},
    .data_size = sizeof(idle_logo_map),
    .data      = idle_logo_map,
}};
""")

    size_kb = len(data) / 1024.0
    print(f"wrote {os.path.normpath(OUTPUT)}")
    print(f"  {width}x{height} RGB565 = {len(data)} bytes ({size_kb:.1f} kB of flash)")
    print("\nBuild with the logo:")
    print("  cmake -S . -B build -DIDLE_LOGO=ON && ninja -C build")


if __name__ == "__main__":
    main()
