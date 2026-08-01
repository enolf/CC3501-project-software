# Assets

Source artwork, and the script that turns it into something the firmware can
compile. Only the idle-screen logo lives here so far.

## Where to put the logo

Save the artwork in this directory as **`logo.png`**.

| Property | What to aim for | Why |
|---|---|---|
| Format | PNG | The converter reads it directly, and it is lossless so edges stay crisp |
| Size | **up to 260 x 170 px** | The panel is 320 x 240; this leaves a margin. Anything larger is scaled down automatically rather than refused |
| Background | Transparent, or solid black | The idle screen is black, and transparency is flattened onto black during conversion |
| Colour | Anything | Converted to RGB565, which is what the display uses |

## Converting it

```
pip install pillow
cd rp2040-software/assets
python convert_logo.py logo.png
```

That writes `../src/assets/idle_logo.c`. Re-run it whenever the artwork changes.

## Building with it

```
cmake -S . -B build -DIDLE_LOGO=ON      # logo on the idle screen
cmake -S . -B build -DIDLE_LOGO=OFF     # black idle screen (the default)
```

The image is only compiled when `IDLE_LOGO=ON`, so a black-screen build carries
none of the artwork. A 260 x 170 logo costs about 86 kB of flash, against
roughly 1.5 MB free.

**Black is the default on purpose.** The terminal spends nearly all its life on
the idle screen, so black means no burn-in and less power, and it reads
unmistakably as "nothing in progress" rather than as a transaction that has got
stuck. The logo is there for demonstrations and for looking presentable in the
society's kitchen.

## If flash ever gets tight

It will not on this board, but for the record: a single-colour logo converted to
1 bit per pixel instead of RGB565 would be about 5 kB rather than 86 kB, and
LVGL can recolour it at draw time. That needs a different output format in
`convert_logo.py` and is not worth doing without a reason.
