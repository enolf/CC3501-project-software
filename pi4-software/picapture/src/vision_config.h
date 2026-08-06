#pragma once

#include <string>
#include <vector>

// Every number that decides what the camera thinks it is looking at.
//
// WHY THIS FILE EXISTS
// --------------------
// The same argument as `board.h` on the firmware side and `config.py` on the
// Pi: one place for a fact, so changing it is an edit rather than a hunt.
//
// It matters more here than anywhere else in the project, because these values
// are not design decisions — they are measurements of a particular fridge under
// particular lighting, and they WILL need changing. Somebody re-tunes them
// standing at the fridge with the door open, which is the worst possible place
// to be editing source and rebuilding. So they load from a file, they save back
// to that file from the debug UI, and the compiled-in values below are only the
// starting point for a machine that has never been tuned.
//
// **If a bare threshold appears anywhere in main.cpp, that is the bug.**
//
// Deliberately free of any OpenCV dependency, so the parsing and round-tripping
// below can be unit-tested on a laptop with no camera and no OpenCV installed —
// exactly the reason basket.cpp has no hardware dependency on the firmware side.

namespace vision {

/// One drink, and the colour it is recognised by.
///
/// THE ORDER OF THE BRAND LIST IS PART OF THE PROTOCOL. Counts are reported
/// positionally and `catalogue::Can` on the RP2040 indexes the same sequence,
/// so inserting a drink in the middle here without the same edit there silently
/// relabels every count. `wire_key` must match `catalogue::wire_key()` exactly.
struct Brand {
    std::string name;      ///< For the debug overlay and for a human reading a log
    std::string wire_key;  ///< What goes on the wire. Lower case, no spaces.

    // The HSV box this drink occupies, in OpenCV's ranges: H is 0-179 (not
    // 0-359 — OpenCV halves it to fit a byte), S and V are 0-255.
    //
    // NOT used as a threshold. The midpoint of each box is the brand's centre,
    // and a pixel is assigned to whichever centre is nearest; lowS and lowV are
    // used as hard floors below which a pixel is too grey or too dark to be
    // this drink at all. See classify_by_nearest_brand() in main.cpp for why
    // nearest-centre replaced per-brand thresholding.
    int lowH = 0, highH = 179;
    int lowS = 0, highS = 255;
    int lowV = 0, highV = 255;
};

struct Config {
    // --- What the camera is looking for ---
    std::vector<Brand> brands;

    /// How much more a hue mismatch counts than a saturation or value one.
    ///
    /// Hue is the only reliable discriminator between these four drinks: S and
    /// V move with glare and shadow across a single can, while hue barely
    /// shifts. Weighting it up is what stops the lit side of a Coke and the
    /// shadowed side of a Fanta from swapping identities.
    double hue_weight = 4.0;

    /// How far a pixel may sit from the nearest brand centre and still be
    /// called that brand. Beyond this it is background — the shelf, the door,
    /// a hand. Raise it and the fridge lining starts being counted as a drink.
    double max_brand_dist = 70.0;

    // --- Cleaning up the mask ---

    /// Opening kernel, in pixels. Removes speckle. 0 disables it.
    ///
    /// NOT ZERO BY DEFAULT, and that is the whole point of this file. It used
    /// to default to zero, which meant a headless run did no morphological
    /// cleanup at all and fed ragged, text-shaped noise straight into the
    /// contour finder. Nothing said so; the counts were simply wrong.
    int open_kernel = 3;

    /// Closing kernel width. The kernel is this wide and seven times as tall,
    /// because a can is a tall thin blob and a glare band across the middle of
    /// one splits it in two — closing vertically joins it back up without
    /// merging two cans standing side by side.
    int close_kernel = 3;

    /// Contour area bounds, in pixels, at the processing resolution below.
    /// Anything smaller is noise; anything larger is two cans that have merged
    /// into one blob, or a reflection of the whole shelf.
    int contour_min_area = 500;
    int contour_max_area = 40000;

    /// Radius of the blur used to estimate the lighting across the scene, for
    /// flat-field correction. Must be comfortably larger than a can, or the
    /// correction starts flattening the cans themselves instead of the light
    /// falling on them. 0 disables the correction.
    int flat_field_blur = 51;

    // --- The camera ---
    //
    // Captured high and processed low on purpose: the sensor gives a better
    // image downsampled than it does asked for a small one directly, and every
    // stage after this is per-pixel work.
    int capture_width = 800;
    int capture_height = 600;
    int process_width = 400;
    int process_height = 300;

    /// Whether the camera is mounted upside-down. It is easier to fix here than
    /// to remount the camera, and it has to be fixed somewhere — a flipped
    /// image still detects cans perfectly well, so this is not self-correcting
    /// and nothing downstream would ever complain.
    bool rotate_180 = true;

    /// How often to look, in milliseconds. Not a frame rate to maximise: this
    /// runs beside `fridged` and Grafana on the same Pi, and the only deadline
    /// that matters is answering a scan inside the board's 8 s recount budget.
    int period_ms = 250;

    // --- Fixed shape constants ---

    /// How many times taller than wide the closing kernel is. Not tunable:
    /// it encodes the fact that a can is a tall thin object, which is a
    /// property of cans rather than of this fridge.
    static constexpr int CLOSE_ASPECT = 7;

    /// Contour simplification tolerance, as a fraction of the contour's own
    /// perimeter. Scaling to the contour is what lets one value work for a can
    /// at the front of the shelf and one at the back.
    static constexpr double APPROX_EPSILON_FRACTION = 0.02;

    // --- Slider ranges for the debug UI ---
    //
    // Here rather than in the UI code so that a value loaded from a file which
    // exceeds its slider cannot silently be clamped to something else the
    // moment somebody opens the debug window.
    static constexpr int MAX_KERNEL = 15;
    static constexpr int MAX_AREA = 60000;
};

/// The compiled-in starting point: four drinks and workable thresholds.
///
/// These came off one set of test images under one set of lights. Treat them as
/// somewhere to start, not as correct — the whole point of the file format
/// below is that correcting them does not need a compiler.
Config defaults();

/// Read `path` over the top of `defaults()`. Keys absent from the file keep
/// their default, so a partial file is valid and a new setting added here does
/// not invalidate everybody's existing config.
///
/// Returns false if the file could not be opened, which is NOT an error — a
/// machine that has never been tuned has no file, and the defaults are correct
/// for it. `error` is set only when the file existed but could not be parsed.
bool load(const std::string &path, Config &out, std::string *error = nullptr);

/// Write `config` to `path` in the same format load() reads, comments and all.
///
/// This is what makes the debug UI worth having. Tuning that cannot be saved is
/// tuning that has to be redone every run, and the temptation is then to
/// hardcode it back into the source — which is where it started.
bool save(const std::string &path, const Config &config,
          std::string *error = nullptr);

/// Snap the values that have to take a particular shape, and say what changed.
///
/// Only the morphology kernels, which must be odd — an even kernel has no
/// centre pixel, so the mask creeps half a pixel every pass. A slider cannot be
/// made to step in odd numbers, so rather than rejecting what the user just
/// dragged, or quietly using a different number than the one on screen, this
/// runs every frame and moves the value itself. What the next frame does and
/// what a save would write are then the same number.
///
/// Returns true if anything moved, with a description in `note` when given.
bool normalise(Config &config, std::string *note);

/// Reject a config that would misbehave rather than letting it run.
///
/// Catches the mistakes that a text file invites and that OpenCV would either
/// throw on or, worse, quietly accept: an even morphology kernel, a min area
/// above the max, an HSV bound outside the range OpenCV actually uses, two
/// drinks sharing a wire key. Returns false and fills `error` with something a
/// person can act on.
bool validate(const Config &config, std::string *error);

} // namespace vision
