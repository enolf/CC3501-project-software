# picapture

Counts the drinks on the shelf from the Pi camera, and prints what it sees.

```
coke:5,fanta:4,mtndew:5,solo:3;
```

One line per look, on stdout, forever. It opens no serial port and talks to
nobody — `fridged` runs it as a subprocess and reads its output, which is what
keeps the serial link owned by exactly one process. Diagnostics go to stderr, so
stdout carries counts and nothing else.

**Requires OpenCV 4.x.** OpenCV 5.0.0 removed the `Moments` class this depends
on. `CMakeLists.txt` pins the major version so the failure says so instead of
producing a wall of template errors.

## Prerequisites

```
sudo apt install libopencv-dev gstreamer1.0-tools \
                 gstreamer1.0-plugins-base-apps gstreamer1.0-libcamera
```

Check the camera and GStreamer independently of this program before blaming it:

```
gst-launch-1.0 libcamerasrc ! video/x-raw, width=800, height=600, framerate=15/1 \
    ! videoconvert ! ximagesink
```

The user running picapture must be in the `video` group.

## Build and run

```
cmake -S . -B build
cmake --build build
./build/PiCapture --headless
```

Three modes, and one of them must be given:

| Mode | What it puts on screen |
| --- | --- |
| `--headless` | Nothing. **No GUI calls at all** — the deployed configuration. |
| `--debug-camera` | The camera view with detections drawn on it. |
| `--debug-all` | ...plus the classified mask, the cleaned mask, the flat-field corrected frame, and the tuning sliders. |

`--headless` genuinely opens no window and never calls `waitKey`, so it runs
under systemd with no display attached. The debug modes need a real display; a
direct HDMI output is much better than VNC or X forwarding.

## Tuning

Every threshold lives in `src/vision_config.h`, and is loaded at startup from
`picapture.conf` in the working directory if that file exists. **There are no
bare numbers in `main.cpp`.**

The compiled-in defaults came off one set of test images under one set of
lights. They are somewhere to start, not something correct. Expect to retune
against the real fridge, and expect to do it again when the lighting changes.

Retuning does not need a compiler:

```
./build/PiCapture --debug-all
```

| Key | What it does |
| --- | --- |
| `1`–`4` | Choose which drink the next two clicks tune |
| click ×2 | Sample **one** can twice — its brightest part, then its dullest |
| `u` | Undo the last sample |
| `r` | Reset the selected drink to its built-in colour |
| `s` | Save the current tuning to `picapture.conf` |
| `p` | Print the current tuning |
| `q` | Quit |

**The drink you are tuning is shown on the camera window.** Check it before
every pair of clicks. The selection persists until you change it, so it is
entirely possible to press `4` for Solo, tune it, then keep clicking on other
cans and quietly retune Solo onto all of them — which drags its centre to a
colour between two drinks, where it starts claiming both.

Two guards exist because that is exactly what happened once:

- Two clicks more than 15 apart in hue **are rejected**, because they cannot be
  the same can. The brand is left alone and the console says so.
- `u` undoes the last sample, one level deep. `r` puts one drink back to its
  built-in colour if it has drifted badly.

In the debug modes, **counts print only when they change** — a steady shelf is
silent, and a wandering one is obvious. Headless still emits every period,
because that is the protocol.

The sliders and the clicks write straight into the live config, so the effect
shows on the very next frame. `s` writes exactly what you are looking at, and
warns if two drinks have ended up too close together to separate.

**Every click also prints why that pixel classified the way it did** — the
distance to each drink, broken down by channel:

```
  sample 1 of 2: H=4 S=241 V=178
      -> Coke           dist   18.3  (hue  12.0  sat   1.8  val  13.5)
         Fanta          dist   31.7  (hue  24.0  sat   2.3  val  20.4)
         Mountain Dew   dist  151.2  (hue 148.0  sat  27.0  val  22.1)
         Solo           dist   72.4  (hue  68.0  sat   2.5  val  22.8)
```

This is the tool for separating two drinks that keep swapping. "Coke sometimes
reads as Fanta" is not actionable; a 13-unit margin with the value term nearly
as large as the hue term tells you exactly what to change. Click the same can in
shade and in glare and watch how much the margin moves.

### How a pixel is classified

Two separate questions, and keeping them separate is load-bearing:

1. **Is this a drink at all?** Global `min_saturation` and `min_value` floors.
   Grey shelf, dark shadow and blown-out highlights are background.
2. **Which drink?** Nearest brand centre, with hue weighted well above
   saturation and value.

These floors used to be **per brand**, and that caused the exact fault it looked
like it was preventing. Coke floored value at 160 and Fanta at 130, so a Coke can
in shadow at V=150 was not rejected as unknown — it was refused by Coke,
accepted by Fanta, and *became a Fanta*. A floor may decide whether something is
a drink; it must never decide which drink.

`sat_weight` and `val_weight` exist for the same reason. With saturation and
value contributing their raw difference, the value gap between Coke and Fanta
(43) exceeded the weighted hue gap (28) — so the channel that tracks the
lighting was out-voting the channel that identifies the drink. `validate()`
refuses a config where either outweighs hue.

### If counts drift with nothing moving

Almost certainly the camera's own auto-exposure and auto-white-balance. As they
adjust, every hue in the frame shifts and classification wanders — for tens of
seconds at a time, which no amount of frame averaging downstream can fix,
because the measurement itself is moving.

`libcamerasrc_extra` is appended to the `libcamerasrc` element verbatim so you
can lock them without a rebuild:

```
libcamerasrc_extra = ae-enable=false awb-mode=daylight
```

The properties available differ between libcamera versions, so check yours
first:

```
gst-inspect-1.0 libcamerasrc
```

Tune the exposure once, with the fridge lit as it will be in service, and lock
it there.

A missing `picapture.conf` is not an error — it just means this machine has
never been tuned. A file that exists but cannot be parsed **is** an error and
the program refuses to start, because running with tuning nobody chose looks
identical to running with tuning that was applied. An unknown key is an error
for the same reason: a silently skipped typo costs an afternoon.

Two settings deserve a note:

- **`open_kernel` and `close_kernel` default to non-zero.** They used to
  default to zero, which meant a headless run did no morphological cleanup at
  all and fed ragged, text-shaped noise to the contour finder. It reported
  counts perfectly happily. Set them to `0` deliberately if you want to see the
  raw mask.
- **`brand` lines replace the built-in list rather than adding to it**, so all
  four have to appear together. Their order is the order counts are reported in
  and it must match `catalogue::Can` in the firmware; each `wire_key` must match
  `catalogue::wire_key()` character for character.

## Tests

`vision_config` has no OpenCV dependency and no camera dependency, deliberately:
the parsing and round-tripping are the parts most likely to be wrong and the
parts least convenient to debug standing at a fridge.

```
cmake --build build --target config_tests
./build/config_tests
```

Runs on any laptop, with or without OpenCV installed.

## Known limits

**A contour is not a can.** The count is the number of blobs of a drink's
colour, so two cans of the same drink touching each other merge into one blob
and report as one. `contour_max_area` only rejects the merged blob, which makes
it report as none instead. Because the firmware charges for the *difference*
between two counts, a merge that appears or disappears between the baseline and
the recount invents or hides a purchase. Separate the cans on the shelf, and
test a full shelf before trusting any of this.

**Lighting differs between the two scans.** The baseline is taken as the door
opens and the recount after it shuts, with the fridge light and the room light
in between. Flat-field correction absorbs some of that. Tune with the door in
the state it will be in when the scan happens, and check that a door cycle with
nothing taken reliably reports no change.
