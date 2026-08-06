#pragma once

#include <algorithm>
#include <cmath>
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
    // NOT a threshold. Only the MIDPOINT of each box is used: it is the brand's
    // centre, and a pixel is assigned to whichever centre is nearest. The
    // width of the box affects nothing on its own.
    //
    // These were per-brand hard floors as well, and that was actively harmful.
    // Coke's box floored value at 160 and Fanta's at 130, so a Coke can in
    // shadow at V=150 was not merely rejected as Coke — it was refused by Coke
    // and accepted by Fanta, and dimming the light turned one drink into
    // another. The floors are now global (see min_saturation / min_value) and
    // decide only whether a pixel is a drink at all, never which drink.
    int lowH = 0, highH = 179;
    int lowS = 0, highS = 255;
    int lowV = 0, highV = 255;
};

struct Config {
    // --- What the camera is looking for ---
    std::vector<Brand> brands;

    // --- How the three channels are traded off against each other ---
    //
    // ALL THREE WEIGHTS MATTER, NOT JUST THE HUE ONE. This was hue_weight
    // alone, with saturation and value contributing their raw difference, and
    // the arithmetic did the opposite of what the comment claimed:
    //
    //     Coke centre  H 3   V 190
    //     Fanta centre H 10  V 147
    //
    //     hue term    (10 - 3) * 4    = 28
    //     value term   190 - 147      = 43
    //
    // So value out-voted hue, and value is precisely the channel that moves
    // when a can is in shadow. Coke and Fanta swapped identities with the
    // lighting. Weighting S and V DOWN is what makes hue actually decide.

    /// Hue's multiplier. The channel that genuinely identifies a drink.
    double hue_weight = 4.0;

    /// Saturation's multiplier. Below 1 because saturation drops off towards
    /// the curved edge of every can, whatever colour it is.
    double sat_weight = 0.5;

    /// Value's multiplier. The lowest of the three: it is almost entirely a
    /// statement about the lighting rather than about the drink.
    double val_weight = 0.25;

    /// How far a pixel may sit from the nearest brand centre and still be
    /// called that brand. Beyond this it is background — the shelf, the door,
    /// a hand. Raise it and the fridge lining starts being counted as a drink.
    ///
    /// Tied to the weights above: lowering sat_weight and val_weight shrinks
    /// every distance, so this came down with them.
    double max_brand_dist = 60.0;

    // --- What counts as a drink at all ---
    //
    // Global, NOT per brand. These decide whether a pixel is coloured enough
    // and lit enough to be any drink; which drink is then settled entirely by
    // distance. Keeping the two questions separate is what stops a shadow from
    // renaming a can — see the note on Brand's HSV box.

    /// Below this saturation a pixel is grey: the shelf, the door seal, a
    /// white label, a specular highlight.
    int min_saturation = 90;

    /// Below this value a pixel is too dark to have a trustworthy hue. Hue is
    /// meaningless in near-black, and quantisation noise there is what
    /// produces speckle in the mask.
    int min_value = 50;

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

    /// Whether the camera is mounted upside-down. Easier to fix here than to
    /// remount the camera, and it has to be fixed somewhere — a flipped image
    /// still detects cans perfectly well, so nothing downstream will ever
    /// complain and it is not self-correcting.
    ///
    /// False for the camera as actually mounted on this fridge. It was true,
    /// which double-flipped an already-upright image.
    bool rotate_180 = false;

    /// Extra properties appended to the `libcamerasrc` element, verbatim.
    ///
    /// EXISTS FOR ONE JOB: turning off the camera's own automatic exposure and
    /// white balance. With them running, the sensor quietly re-white-balances
    /// as the scene changes, every hue in the frame shifts, and counts wander
    /// for tens of seconds at a time with nothing in front of the camera
    /// having moved. That is not noise a settling filter can average away —
    /// it is the measurement itself drifting.
    ///
    /// A passthrough rather than named settings because the properties
    /// `libcamerasrc` accepts differ between libcamera versions, and a
    /// hardcoded name that this build does not recognise is a startup failure
    /// on somebody else's Pi. Run `gst-inspect-1.0 libcamerasrc` to see what
    /// yours takes.
    std::string libcamerasrc_extra;

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

    /// The most two click-samples may differ in hue and still be believed to
    /// come from the same can.
    ///
    /// A guard against the tuning UI's easiest mistake, which is not
    /// hypothetical: with Solo still armed, four clicks across a Coke, a Fanta
    /// and a Mountain Dew moved Solo's centre to hue 37 — between Solo and
    /// Dew — and it began claiming part of the Dew can. Nothing complained,
    /// and the result was saved.
    ///
    /// Generous enough for the lit and shadowed sides of one can, which is
    /// what a pair is supposed to be. Anything wider is two different drinks.
    static constexpr int MAX_SAMPLE_HUE_SPREAD = 15;

    /// How close two brand centres may sit, in weighted distance, before the
    /// pair is too alike to tell apart reliably. Only a warning: two similar
    /// drinks might genuinely be the best available, and refusing to save
    /// would leave nowhere to go.
    static constexpr double MIN_CENTRE_SEPARATION = 25.0;

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

// --- Hue arithmetic -------------------------------------------------------
//
// Here rather than in main.cpp because hue wraps, and every mistake that
// follows from forgetting so is silent. Being OpenCV-free, these are also the
// only part of the colour handling that can be tested without a camera.

/// Distance between two hues, the short way round.
///
/// OpenCV's hue wraps at 180, not 360. Red sits at both ends of that range, so
/// a plain subtraction makes hue 2 and hue 178 look 176 apart when they are 4
/// apart and both Coke.
///
/// Inline because the classifier calls it for every pixel of every frame
/// against every brand.
inline double hue_distance(double a, double b)
{
    const double d = std::fabs(a - b);
    return std::min(d, 180.0 - d);
}

/// The hue halfway between two hues, going the short way round.
///
/// NOT the arithmetic mean. Coke sits at hue 0, so its pixels land on both
/// sides of the wrap point: samples of 178 and 2 are 4 apart and their true
/// midpoint is 0, but averaging gives 90 — a completely different colour, and
/// one that would then be saved as Coke's identity.
double circular_hue_mean(double a, double b);

/// Could two sampled hues have come from the same can?
///
/// The guard on the tuning UI's easiest mistake. See MAX_SAMPLE_HUE_SPREAD.
bool samples_agree(int hue_a, int hue_b);

/// Set a brand's HSV box from two sampled pixels, with padding.
///
/// Takes plain integers rather than an OpenCV type so this file stays
/// camera-free and the wrap-around behaviour above can be tested directly.
void apply_sample(Brand &brand, int h1, int s1, int v1,
                  int h2, int s2, int v2);

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
