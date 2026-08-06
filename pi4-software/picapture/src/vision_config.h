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

    // This drink's colour, in OpenCV's ranges: H is 0-179 (not 0-359 — OpenCV
    // halves it to fit a byte), S and V are 0-255.
    //
    // A SINGLE POINT, not a range, because a single point is all the
    // classifier has ever used: a pixel is assigned to whichever brand centre
    // it is nearest.
    //
    // It was stored as a low/high band, and that was not merely redundant — it
    // was lossy in a way that cost real accuracy. The centre was recovered as
    // the band's midpoint, and the band was clamped to 0-179, so a drink
    // sitting at hue 0 could not be represented at all: sampling Coke at hue 0
    // produced the band 0-6 and therefore the centre 3. Three hue of Coke's
    // separation from Fanta was thrown away by the storage format, in the
    // direction of the drink it was already hardest to tell apart.
    int hue = 0;
    int saturation = 128;
    int value = 128;

    /// Area of ONE can of this drink, in pixels at the processing resolution.
    /// Zero means "not calibrated", and counting falls back to one can per
    /// blob.
    ///
    /// WHY COUNTING BLOBS IS NOT ENOUGH
    /// --------------------------------
    /// A blob is a connected region of one colour, and two cans of the same
    /// drink standing against each other are one connected region. Counting
    /// blobs reports them as one can.
    ///
    /// That is not a cosmetic error. The firmware charges for the DIFFERENCE
    /// between the shelf before a door cycle and after it, so a merge that
    /// appears or disappears between those two scans invents or hides a
    /// purchase — and nothing anywhere reports a fault, because both counts
    /// were perfectly well-formed.
    ///
    /// Tightening contour_max_area does not help: it makes a merged pair count
    /// as zero rather than as two, which is further from the truth.
    ///
    /// Dividing by the area of one can does help, because area is very nearly
    /// linear in the number of cans while blob count is not. Per brand rather
    /// than global because the cans are the same size but the coloured PART of
    /// each is not — a Coke is red nearly all over, a Mountain Dew has a wide
    /// pale band.
    ///
    /// Calibrate with the `c` key: show exactly one can and press it.
    int can_area = 0;
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
    ///
    /// `contour_min_area` is the single most effective stabiliser in the whole
    /// pipeline, and it was set far too low. At 500 — under a tenth of a can —
    /// every label fragment, reflection and sliver of the wrong-coloured edge
    /// was counted, and the counts flickered constantly. Raising it to roughly
    /// half a can removed nearly all of that.
    ///
    /// `contour_max_area` is a sanity bound, NOT a merge detector. Two touching
    /// cans make one blob of about twice the area, and rejecting it counts them
    /// as zero rather than as two. `Brand::can_area` is what handles merges;
    /// leave this generous enough that a merged pair still gets through to be
    /// divided up.
    ///
    /// Both are in pixels at the processing resolution, so both change if
    /// process_width/height change, and both depend on how far the camera sits
    /// from the shelf. Measure them with the `a` key rather than guessing.
    int contour_min_area = 2500;
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

    /// Half-width of the square of pixels each click contributes, in pixels.
    ///
    /// Large enough to average away one bad pixel and to span a real piece of
    /// the can; small enough that a click near the edge does not swallow the
    /// background behind it.
    static constexpr int SAMPLE_PATCH_RADIUS = 6;

    /// How close two brand centres may sit before the pair is too alike to
    /// separate reliably, as a fraction of `max_brand_dist`.
    ///
    /// Relative rather than absolute because `max_brand_dist` is the radius
    /// within which a pixel is accepted as a brand at all: two centres closer
    /// together than that have heavily overlapping acceptance regions, and the
    /// pixels in the overlap go to whichever is marginally nearer — which
    /// shadow and glare can flip.
    ///
    /// An absolute 25 was the first attempt and it was too lax to be useful.
    /// It stayed silent while Mountain Dew's centre drifted to within 9 hue of
    /// Solo's, which is exactly the situation it existed to catch.
    ///
    /// Only ever a warning. Two similar drinks may genuinely be what is being
    /// stocked, and refusing to save would leave nowhere to go.
    static constexpr double MIN_CENTRE_SEPARATION_FRACTION = 0.75;

    // --- "The picture is wrong, not the tuning" detector ---
    //
    // A dark frame and an empty shelf produce the SAME output: all zeros. That
    // is a genuinely bad failure mode, because the natural reading is that
    // detection has broken and the natural response is to retune — which
    // cannot help and quietly makes the tuning worse.
    //
    // It is not hypothetical. Setting `ae-enable=false` on libcamerasrc without
    // also supplying `exposure-time` and `analogue-gain` puts the sensor into
    // manual mode with no values to use, and the picture goes nearly black.
    // Every count went to zero and nothing said why.

    /// Below this fraction of pixels passing the floors, the frame is
    /// effectively empty.
    static constexpr double DARK_FRAME_CANDIDATE_FRACTION = 0.005;

    /// How many consecutive such frames before saying so. A few seconds' worth,
    /// so genuinely closing the fridge on an empty shelf does not trip it.
    static constexpr int DARK_FRAME_WARN_AFTER = 20;

    /// How often to repeat the warning while it persists. Often enough not to
    /// be missed, rarely enough not to become the log.
    static constexpr int DARK_FRAME_WARN_REPEAT = 240;

    // --- How much this frame is worth believing ---
    //
    // Every count line carries a `conf=` figure, and these are the deductions
    // that produce it. Deductions from 100 rather than factors multiplied
    // together, because the number has to be explainable: "60, because two
    // drinks are uncalibrated and a blob was thrown away" is actionable, and
    // "0.6" is not.
    //
    // The whole point is that a wrong count is otherwise INVISIBLE. Every other
    // fault in this system announces itself — a bad frame fails its CRC, a
    // missing payment has no row. A miscounted shelf produces a perfectly
    // well-formed packet with the wrong numbers in it, and the only evidence
    // available is how equivocal the measurement was.

    /// Deducted when no brand has had its can area measured, scaled by the
    /// fraction that have not. An uncalibrated brand counts one can per blob,
    /// so it cannot notice two touching cans and cannot contribute the area
    /// evidence below. It should say so rather than look as sure as a
    /// calibrated one.
    static constexpr double CONF_UNCALIBRATED_PENALTY = 20.0;

    /// Deducted for a blob sitting exactly halfway between two whole numbers of
    /// cans, scaled down to zero for a blob at a whole number. The largest of
    /// the deductions because it is the most direct evidence there is: a blob
    /// measuring 1.5 cans genuinely could be either, and whichever way it
    /// rounds decides whether somebody is charged.
    static constexpr double CONF_AMBIGUITY_PENALTY = 50.0;

    /// Deducted per blob thrown out for being too small or too large, up to the
    /// cap below. A rejected blob is a thing the camera saw and could not
    /// explain; it is usually noise, and occasionally it is a can at an angle.
    static constexpr double CONF_REJECT_PENALTY = 8.0;
    static constexpr double CONF_REJECT_PENALTY_MAX = 24.0;

    /// How large a discarded blob has to be, as a fraction of
    /// `contour_min_area`, before it counts against confidence at all.
    ///
    /// Without this the deduction would sit at its cap permanently and carry no
    /// information: any real mask contains dozens of few-pixel fragments per
    /// frame — a letter of a label, an edge, a speck of the wrong colour — and
    /// every one of them is a discard. None of them is evidence about the
    /// count. A blob at four fifths of a can very much is.
    static constexpr double CONF_NOTABLE_REJECT_FRACTION = 0.5;

    /// Deducted for an exposure right at the end of its range, ramping in from
    /// the bounds below. Also large, because outside these bounds hue is not
    /// measured so much as guessed.
    static constexpr double CONF_EXPOSURE_PENALTY = 60.0;

    /// The mean-brightness band within which no exposure deduction is taken.
    static constexpr double CONF_VALUE_FLOOR = 40.0;
    static constexpr double CONF_VALUE_CEILING = 215.0;

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

/// A collection of pixels sampled off one can, and the colour they average to.
///
/// WHY A SET AND NOT TWO PIXELS
/// ----------------------------
/// Tuning used to take exactly two clicked pixels and treat them as the whole
/// truth about a drink. Two pixels is not a measurement of a can, and the
/// failures were not subtle:
///
///   * Clicking twice on nearly the same spot pinned the colour to that spot.
///     Mountain Dew spans about ten hue across one can; two clicks that both
///     landed at hue 39 put its centre at 39, and the hue-29 half of the can
///     then sat outside `max_brand_dist` and stopped being detected at all.
///   * A single unlucky pixel — a glare speck, a JPEG artefact, the dark line
///     between two cans — carried the same weight as a representative one.
///
/// Each click now contributes a patch of pixels and the centre is their
/// median, so one bad pixel changes nothing and the centre lands in the middle
/// of the colour the can actually is.
class SampleSet {
public:
    void add(int hue, int saturation, int value);
    void clear();
    size_t size() const { return hues_.size(); }
    bool empty() const { return hues_.empty(); }

    /// The hue these samples represent: a circular median, robust to the
    /// stray bright or dark pixel that any patch off a curved can will catch.
    /// Zero if empty.
    double centre_hue() const;

    /// How far the outlying samples sit from that mean, in hue. The figure
    /// worth looking at when a drink will not hold still: a spread wider than
    /// the gap to the next drink cannot be tuned away.
    double hue_spread() const;

    /// Write the sampled colour into `brand`, leaving its name, wire key and
    /// can area alone.
    ///
    /// False if there is nothing to write, or if the samples are too spread out
    /// in hue to be one can — which is what clicking on two different drinks
    /// looks like from in here.
    bool to_brand(Brand &brand, std::string *error) const;

private:
    std::vector<int> hues_;
    std::vector<int> saturations_;
    std::vector<int> values_;
};

// --- How much this frame is worth believing -------------------------------

/// What was observed about one frame, as far as its trustworthiness goes.
///
/// Filled in by the pipeline, turned into a number by `confidence()` below.
/// Separated from the OpenCV code for the usual reason — the arithmetic is what
/// is worth testing, and it needs no camera to test.
///
/// THIS IS A PER-FRAME FIGURE. It says how well-formed this one look was. It is
/// NOT the confidence eventually reported to the board, which also has to
/// account for whether successive frames agreed with each other. One says "the
/// picture was good"; the other says "the shelf held still". Conflating them
/// would let a run of identically bad frames look like a settled answer.
struct Quality {
    /// How many brands were looked for, and how many of those have no
    /// `can_area` measured.
    int brands = 0;
    int uncalibrated = 0;

    /// How cleanly the counted blobs divide into whole cans. Only calibrated
    /// brands contribute — without a can area there is nothing to divide by,
    /// which is why being uncalibrated is a deduction in its own right rather
    /// than free.
    int ambiguity_blobs = 0;
    double ambiguity_sum = 0.0;   ///< Each term 0 (a whole can) to 0.5 (halfway)

    /// Blobs discarded by the area filter. Kept apart because they mean
    /// different things: lots of small ones is noise or a tuning problem, a
    /// large one is usually cans merging beyond what the filter allows.
    int rejected_small = 0;
    int rejected_large = 0;

    /// Mean brightness of the frame, 0-255.
    ///
    /// The candidate-pixel fraction is deliberately NOT an input here, even
    /// though it is what the dark-frame detector uses. An empty shelf honestly
    /// has almost no candidate pixels, and reporting low confidence for
    /// correctly seeing nothing would make the figure mean two different things
    /// at once. Brightness separates the two cases, which is exactly the job it
    /// already does for the dark-frame warning.
    double mean_value = 0.0;
};

/// How much to believe this frame, 0-100.
///
/// With `why`, also writes the arithmetic out — every deduction taken, in
/// words. That readout is the point: a bare number tells you something is
/// wrong, and the breakdown tells you which knob it is about.
int confidence(const Quality &quality, std::string *why = nullptr);

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
