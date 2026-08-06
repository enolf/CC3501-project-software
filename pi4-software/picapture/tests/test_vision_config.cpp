// Tests for picapture's tuning file: defaults, parsing, round trip, refusals.
//
// No OpenCV and no camera. vision_config.cpp is kept free of both precisely so
// that this can run anywhere — the same argument basket.cpp makes on the
// firmware side, and for the same reason: this is the part most likely to be
// wrong and the part least convenient to debug standing at a fridge.

#include "vision_config.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

int checks = 0;
int failures = 0;

void check(const std::string &label, bool condition)
{
    checks++;
    if (!condition) failures++;
    printf("  %s  %s\n", condition ? "ok  " : "FAIL", label.c_str());
}

void suite(const char *name)
{
    printf("\n--- %s ---\n", name);
}

/// A scratch file that cleans up after itself, so a failing test cannot leave
/// state behind that makes the NEXT run pass or fail for the wrong reason.
struct TempFile {
    std::string path;
    explicit TempFile(const char *name) : path(std::string("test_") + name)
    {
        std::remove(path.c_str());
    }
    ~TempFile() { std::remove(path.c_str()); }

    void write(const std::string &contents) const
    {
        std::ofstream file(path, std::ios::trunc);
        file << contents;
    }
};

void test_defaults()
{
    suite("built-in defaults");

    const vision::Config config = vision::defaults();
    std::string error;

    check("the defaults are self-consistent", vision::validate(config, &error));
    if (!error.empty()) printf("        %s\n", error.c_str());

    check("four drinks are configured", config.brands.size() == 4);

    // The order is the protocol: catalogue::Can on the RP2040 indexes this same
    // sequence positionally, so a drink inserted in the middle here silently
    // relabels every count.
    const std::vector<std::string> expected = {"coke", "fanta", "mtndew",
                                               "solo"};
    bool order_ok = config.brands.size() == expected.size();
    for (size_t i = 0; order_ok && i < expected.size(); i++) {
        order_ok = config.brands[i].wire_key == expected[i];
    }
    check("the wire keys match catalogue::Can's order", order_ok);

    // The bug this file was created to prevent: Open and Close used to default
    // to zero, so a headless run did no morphological cleanup whatsoever and
    // fed ragged noise to the contour finder. It reported counts happily.
    check("morphological cleanup is on by default",
          config.open_kernel > 0 && config.close_kernel > 0);

    // Hue is the only channel that identifies a drink rather than the light
    // falling on it. When saturation and value were unweighted, the value term
    // between Coke and Fanta (43) exceeded the weighted hue term (28), so a can
    // in shadow changed drink. The ordering below is the fix.
    check("hue outweighs both saturation and value",
          config.hue_weight > config.sat_weight &&
          config.hue_weight > config.val_weight);

    // Value is the channel that moves most with lighting and says least about
    // which drink it is, so it should be the least trusted of the three.
    check("value is trusted least of all",
          config.val_weight <= config.sat_weight);

    check("the candidate floors are set",
          config.min_saturation > 0 && config.min_value > 0);

    // 500 was under a tenth of a can, so every label fragment and reflection
    // was counted and the counts flickered constantly. Raising it was the
    // single most effective stabiliser found on real hardware.
    check("the minimum blob area is a meaningful fraction of a can",
          config.contour_min_area >= 1500);

    // Uncalibrated by default: a divisor nobody measured would be a guess
    // applied to every count, and one-per-blob is at least honest about what
    // it is doing.
    bool uncalibrated = true;
    for (const vision::Brand &brand : config.brands) {
        if (brand.can_area != 0) uncalibrated = false;
    }
    check("can areas start uncalibrated rather than guessed", uncalibrated);

    // The camera on this fridge produces an upright image. rotate_180 was
    // true, which flipped an already-correct frame.
    check("rotation is off for the camera as mounted", !config.rotate_180);
}

void test_round_trip()
{
    suite("save and load round trip");

    TempFile file("round_trip.conf");

    vision::Config original = vision::defaults();
    original.open_kernel = 5;
    original.close_kernel = 7;
    original.contour_min_area = 1234;
    original.contour_max_area = 23456;
    original.hue_weight = 3.5;
    original.sat_weight = 0.75;
    original.val_weight = 0.125;
    original.max_brand_dist = 61.25;
    original.min_saturation = 77;
    original.min_value = 44;
    original.rotate_180 = true;
    original.period_ms = 400;
    // A GStreamer fragment: spaces, equals signs and all. It has to survive
    // verbatim or locking the camera's exposure silently stops working.
    original.libcamerasrc_extra = "auto-focus-mode=manual ae-enable=false";
    original.brands[2].hue = 44;
    original.brands[1].can_area = 5400;
    // Hue 0 is the case the old low/high band could not store at all, so it is
    // the one worth pinning through a round trip.
    original.brands[0].hue = 0;

    std::string error;
    check("saving succeeds", vision::save(file.path, original, &error));
    if (!error.empty()) printf("        %s\n", error.c_str());

    vision::Config reloaded;
    check("loading succeeds", vision::load(file.path, reloaded, &error));
    if (!error.empty()) printf("        %s\n", error.c_str());

    check("integers survive", reloaded.open_kernel == 5 &&
                              reloaded.close_kernel == 7 &&
                              reloaded.contour_min_area == 1234 &&
                              reloaded.contour_max_area == 23456 &&
                              reloaded.min_saturation == 77 &&
                              reloaded.min_value == 44 &&
                              reloaded.period_ms == 400);
    check("doubles survive", reloaded.hue_weight == 3.5 &&
                             reloaded.sat_weight == 0.75 &&
                             reloaded.val_weight == 0.125 &&
                             reloaded.max_brand_dist == 61.25);
    check("booleans survive", reloaded.rotate_180 == true);

    // Written last on the line and containing both spaces and '=', so it is
    // the field most likely to be mangled by a naive parser.
    check("a GStreamer fragment survives verbatim, spaces and all",
          reloaded.libcamerasrc_extra ==
              "auto-focus-mode=manual ae-enable=false");

    bool brands_ok = reloaded.brands.size() == original.brands.size();
    for (size_t i = 0; brands_ok && i < original.brands.size(); i++) {
        const vision::Brand &a = original.brands[i];
        const vision::Brand &b = reloaded.brands[i];
        brands_ok = a.name == b.name && a.wire_key == b.wire_key &&
                    a.hue == b.hue && a.saturation == b.saturation &&
                    a.value == b.value && a.can_area == b.can_area;
    }
    check("every brand survives, name, colour and can area intact", brands_ok);

    // The failure the band format made unavoidable: a drink at hue 0 came back
    // as hue 3, three points nearer the drink it was hardest to separate from.
    check("a drink at hue 0 comes back at hue 0, not shifted off the wrap",
          reloaded.brands[0].hue == 0);

    // "Mountain Dew" has a space in it. The name is separated by '|' rather
    // than whitespace for exactly this reason, and a round trip is the only
    // thing that proves it.
    check("a multi-word drink name survives the round trip",
          reloaded.brands[2].name == "Mountain Dew");
}

void test_partial_and_missing()
{
    suite("partial files and missing files");

    std::string error;
    vision::Config config;

    // No file is the normal state of a machine nobody has tuned yet. It must
    // not be an error, and it must be distinguishable from a broken file.
    check("a missing file is not an error",
          !vision::load("test_definitely_not_here.conf", config, &error) &&
          error.empty());

    TempFile file("partial.conf");
    file.write("# only one setting\nopen_kernel = 7\n");

    error.clear();
    check("a partial file loads", vision::load(file.path, config, &error));
    check("the setting it names is applied", config.open_kernel == 7);
    check("everything else keeps its default",
          config.close_kernel == vision::defaults().close_kernel &&
          config.brands.size() == 4);

    // Listing any brand replaces the whole list. Appending instead would leave
    // the old colour in place, and every can would be claimed twice.
    TempFile brands("one_brand.conf");
    brands.write("brand = Coke | coke | 0,6,220,255,160,220\n");
    error.clear();
    check("a file listing brands loads", vision::load(brands.path, config,
                                                      &error));
    check("listing a brand REPLACES the built-in list rather than appending",
          config.brands.size() == 1);

    // Per-brand can areas were added after people already had tuning files.
    // Refusing a three-field brand line would mean everybody redoing their
    // colour tuning to gain a feature that is itself opt-in.
    check("a brand line written before can areas existed still loads",
          config.brands.size() == 1 && config.brands[0].can_area == 0);

    TempFile with_area("brand_area.conf");
    with_area.write("brand = Coke | coke | 0,6,220,255,160,220 | 5400\n");
    error.clear();
    check("a brand line with a can area loads",
          vision::load(with_area.path, config, &error) &&
          config.brands.size() == 1 && config.brands[0].can_area == 5400);

    // --- The old six-number band converts to a colour ---
    //
    // Those files hold real tuning that somebody stood at a fridge to produce.
    // Refusing them would mean redoing the colour work to gain a change that
    // was made FOR the colour work.
    TempFile legacy("legacy_band.conf");
    legacy.write("brand = Fanta | fanta | 4,16,240,255,190,220\n");
    error.clear();
    check("an old low/high band still loads",
          vision::load(legacy.path, config, &error));
    check("...converted to the midpoint the classifier always used",
          config.brands.size() == 1 && config.brands[0].hue == 10 &&
          config.brands[0].saturation == 247 &&
          config.brands[0].value == 205);

    // A three-number line is the new form and must not be mistaken for a
    // truncated old one.
    TempFile modern("modern_colour.conf");
    modern.write("brand = Coke | coke | 0,247,190\n");
    error.clear();
    check("a three-number colour loads as a colour, not a broken band",
          vision::load(modern.path, config, &error) &&
          config.brands.size() == 1 && config.brands[0].hue == 0 &&
          config.brands[0].saturation == 247 &&
          config.brands[0].value == 190);
}

void test_refusals()
{
    suite("bad input is refused, not absorbed");

    vision::Config config;
    std::string error;

    struct Case {
        const char *label;
        const char *contents;
    };

    const Case cases[] = {
        {"an unknown key is refused rather than ignored",
         "open_kernal = 3\n"},
        {"a line with no '=' is refused",
         "open_kernel 3\n"},
        {"a non-numeric value is refused rather than read as zero",
         "open_kernel = three\n"},
        {"a partly-numeric value is refused rather than half-read",
         "contour_min_area = 500px\n"},
        {"a brand with the wrong number of fields is refused",
         "brand = Coke | coke\n"},
        {"a brand with the wrong number of HSV values is refused",
         "brand = Coke | coke | 0,6,220,255\n"},
        {"a brand with an empty wire key is refused",
         "brand = Coke |  | 0,6,220,255,160,220\n"},
    };

    for (const Case &item : cases) {
        TempFile file("refusal.conf");
        file.write(item.contents);
        error.clear();
        const bool loaded = vision::load(file.path, config, &error);
        check(item.label, !loaded && !error.empty());
    }

    // An unknown key deserves special mention: ignoring it is the tempting
    // behaviour and the wrong one. A typo that is silently skipped means
    // somebody spends an afternoon wondering why their change did nothing.
    TempFile typo("typo.conf");
    typo.write("open_kernal = 3\n");
    error.clear();
    vision::load(typo.path, config, &error);
    check("the error names the offending key",
          error.find("open_kernal") != std::string::npos);
}

void test_validation()
{
    suite("validation");

    std::string error;

    struct Case {
        const char *label;
        void (*mutate)(vision::Config &);
    };

    const Case cases[] = {
        {"hue above 179 is rejected (OpenCV halves the angle)",
         [](vision::Config &c) { c.brands[0].hue = 200; }},
        {"a saturation outside 0-255 is rejected",
         [](vision::Config &c) { c.brands[0].saturation = 300; }},
        // A colour the global floors would reject can never be matched, so
        // this drink would silently never be detected at all.
        {"a colour below the candidate floors is rejected",
         [](vision::Config &c) { c.brands[0].value = c.min_value - 1; }},
        {"a min area above the max is rejected",
         [](vision::Config &c) { c.contour_min_area = c.contour_max_area + 1; }},
        {"an even morphology kernel is rejected",
         [](vision::Config &c) { c.open_kernel = 4; }},
        {"an even flat-field blur is rejected",
         [](vision::Config &c) { c.flat_field_blur = 50; }},
        {"a zero hue weight is rejected",
         [](vision::Config &c) { c.hue_weight = 0.0; }},
        {"a negative weight is rejected",
         [](vision::Config &c) { c.val_weight = -1.0; }},
        // The exact configuration that made Coke read as Fanta in shadow.
        // Rejected rather than allowed as a tuning choice, because it defeats
        // the one principle the whole classifier rests on.
        {"value outweighing hue is rejected",
         [](vision::Config &c) { c.val_weight = c.hue_weight + 1.0; }},
        {"saturation outweighing hue is rejected",
         [](vision::Config &c) { c.sat_weight = c.hue_weight; }},
        {"a saturation floor above 255 is rejected",
         [](vision::Config &c) { c.min_saturation = 300; }},
        {"a negative can area is rejected",
         [](vision::Config &c) { c.brands[0].can_area = -1; }},
        // A can smaller than the smallest blob worth counting means every
        // accepted blob divides to two or more cans, and the whole shelf
        // silently doubles.
        {"a can area below contour_min_area is rejected",
         [](vision::Config &c) {
             c.brands[0].can_area = c.contour_min_area - 1;
         }},
        {"upscaling past the capture resolution is rejected",
         [](vision::Config &c) { c.process_width = c.capture_width * 2; }},
        {"a period near the board's recount budget is rejected",
         [](vision::Config &c) { c.period_ms = 6000; }},
        {"an empty brand list is rejected",
         [](vision::Config &c) { c.brands.clear(); }},
        {"a wire key with a space is rejected",
         [](vision::Config &c) { c.brands[0].wire_key = "mountain dew"; }},
        {"an upper-case wire key is rejected",
         [](vision::Config &c) { c.brands[0].wire_key = "Coke"; }},
        {"two brands sharing a wire key is rejected",
         [](vision::Config &c) { c.brands[1].wire_key = c.brands[0].wire_key; }},
    };

    for (const Case &item : cases) {
        vision::Config config = vision::defaults();
        item.mutate(config);
        error.clear();
        check(item.label, !vision::validate(config, &error) && !error.empty());
    }
}

void test_normalise()
{
    suite("normalisation");

    vision::Config config = vision::defaults();
    config.open_kernel = 4;
    config.close_kernel = 6;

    std::string note;
    check("an even kernel is reported as moved",
          vision::normalise(config, &note));
    check("both kernels are snapped up to odd",
          config.open_kernel == 5 && config.close_kernel == 7);
    check("the note says what changed",
          note.find("open_kernel") != std::string::npos &&
          note.find("close_kernel") != std::string::npos);

    // Slider input is normalised every frame, so it has to be cheap AND stable:
    // normalising an already-normal config must report no change, or the debug
    // console would scroll a message every frame forever.
    note.clear();
    check("normalising twice is a no-op", !vision::normalise(config, &note));

    // Zero means "disabled" and must survive, or turning morphology off to see
    // the raw mask would silently turn it back on at kernel 1.
    config.open_kernel = 0;
    check("zero stays zero rather than becoming 1",
          !vision::normalise(config, nullptr) && config.open_kernel == 0);

    // The point of normalising every frame is that whatever a slider produces
    // is then acceptable to validate(), so pressing save can never be refused
    // for a reason the user cannot see or act on.
    vision::Config from_slider = vision::defaults();
    from_slider.open_kernel = 8;
    from_slider.close_kernel = 12;
    vision::normalise(from_slider, nullptr);
    note.clear();
    check("anything a slider can produce validates once normalised",
          vision::validate(from_slider, &note));
    if (!note.empty()) printf("        %s\n", note.c_str());
}

/// A frame with nothing wrong with it: everything calibrated, blobs landing on
/// whole cans, nothing discarded, sensibly exposed.
vision::Quality perfect_frame()
{
    vision::Quality quality;
    quality.brands = 4;
    quality.uncalibrated = 0;
    quality.ambiguity_blobs = 5;
    quality.ambiguity_sum = 0.0;
    quality.mean_value = 130.0;
    return quality;
}

void test_confidence()
{
    suite("confidence");

    std::string why;
    check("a clean frame is fully trusted",
          vision::confidence(perfect_frame(), &why) == 100);
    check("the reasoning is written out even when nothing was deducted",
          why.find("100") != std::string::npos);

    // --- Nothing to compare against ---
    //
    // The default config ships with every can area at zero, so this is what a
    // freshly built, never-tuned Pi reports. It should NOT look like a frame
    // somebody checked and was happy with.
    vision::Quality uncalibrated = perfect_frame();
    uncalibrated.uncalibrated = 4;
    uncalibrated.ambiguity_blobs = 0;
    uncalibrated.ambiguity_sum = 0.0;
    check("a wholly uncalibrated frame is not fully trusted",
          vision::confidence(uncalibrated) < 100);

    vision::Quality half = uncalibrated;
    half.uncalibrated = 2;
    check("calibrating half the drinks recovers half the deduction",
          vision::confidence(half) > vision::confidence(uncalibrated) &&
          vision::confidence(half) < 100);

    // --- The blob that could be one can or two ---
    //
    // The measurement this whole figure exists for. A blob at 1.5x the size of
    // one can rounds to two and could just as well have been one, and nothing
    // else anywhere in the pipeline can tell that apart from a blob at 2.0x:
    // both produce a well-formed packet saying "2".
    vision::Quality ambiguous = perfect_frame();
    ambiguous.ambiguity_blobs = 1;
    ambiguous.ambiguity_sum = 0.5;
    why.clear();
    const int worst = vision::confidence(ambiguous, &why);
    check("a blob halfway between one can and two is heavily distrusted",
          worst <= 50);
    check("and the reason names the area evidence",
          why.find("whole number") != std::string::npos);

    vision::Quality slightly_off = perfect_frame();
    slightly_off.ambiguity_blobs = 1;
    slightly_off.ambiguity_sum = 0.05;
    check("a blob nearly on a whole can is barely penalised",
          vision::confidence(slightly_off) >= 90 &&
          vision::confidence(slightly_off) < 100);

    // Ambiguity is a mean, not a sum: one doubtful blob among twenty good ones
    // is a far better frame than one doubtful blob on its own, and a sum would
    // score them identically.
    vision::Quality diluted = perfect_frame();
    diluted.ambiguity_blobs = 20;
    diluted.ambiguity_sum = 0.5;
    check("one doubtful blob among many scores better than one on its own",
          vision::confidence(diluted) > vision::confidence(ambiguous));

    // --- Things seen and not accounted for ---
    vision::Quality rejects = perfect_frame();
    rejects.rejected_small = 1;
    check("a discarded blob costs something",
          vision::confidence(rejects) < 100);

    vision::Quality many_rejects = perfect_frame();
    many_rejects.rejected_small = 50;
    many_rejects.rejected_large = 50;
    // Capped, because otherwise a frame with a noisy mask would score zero and
    // drown out the area evidence, which is the part that actually bears on
    // whether the count is right.
    check("the discard deduction is capped rather than unbounded",
          vision::confidence(many_rejects) >=
              (int)(100.0 - vision::Config::CONF_REJECT_PENALTY_MAX -
                    vision::Config::CONF_UNCALIBRATED_PENALTY));

    // --- Exposure ---
    //
    // The failure that actually happened: ae-enable=false with no exposure time
    // blacked out the picture, every count went to zero, and the packets were
    // perfectly well-formed. Confidence has to be the thing that notices.
    vision::Quality dark = perfect_frame();
    dark.mean_value = 3.0;
    dark.ambiguity_blobs = 0;
    why.clear();
    check("a nearly black frame is heavily distrusted",
          vision::confidence(dark, &why) < 50);
    check("and says the picture was dark rather than blaming the tuning",
          why.find("dark") != std::string::npos);

    vision::Quality washed_out = perfect_frame();
    washed_out.mean_value = 252.0;
    check("a blown-out frame is distrusted too",
          vision::confidence(washed_out) < 50);

    // Ramped, not a cliff: there is no brightness at which hue abruptly stops
    // meaning anything, so a frame just below the floor must not score the same
    // as a black one.
    vision::Quality dim = perfect_frame();
    dim.mean_value = vision::Config::CONF_VALUE_FLOOR - 5.0;
    check("dim scores between well-lit and black",
          vision::confidence(dim) < 100 &&
          vision::confidence(dim) > vision::confidence(dark));

    vision::Quality just_inside = perfect_frame();
    just_inside.mean_value = vision::Config::CONF_VALUE_FLOOR;
    check("exactly at the floor is not yet penalised",
          vision::confidence(just_inside) == 100);

    // --- The number stays a percentage ---
    //
    // Deductions are independent and several can apply at once, so their total
    // can exceed 100. A negative confidence would be nonsense on a dashboard
    // and would break any downstream threshold.
    vision::Quality everything_wrong;
    everything_wrong.brands = 4;
    everything_wrong.uncalibrated = 4;
    everything_wrong.ambiguity_blobs = 3;
    everything_wrong.ambiguity_sum = 1.5;
    everything_wrong.rejected_small = 20;
    everything_wrong.rejected_large = 5;
    everything_wrong.mean_value = 0.0;
    const int floor_value = vision::confidence(everything_wrong);
    check("confidence never goes below zero", floor_value >= 0);
    check("an entirely broken frame reports close to zero", floor_value <= 10);

    // An empty shelf is not a bad frame. Reporting "none of anything" correctly
    // has to score well, or the figure would mean two different things at once
    // and no threshold could be set on it.
    vision::Quality empty_shelf;
    empty_shelf.brands = 4;
    empty_shelf.uncalibrated = 0;
    empty_shelf.mean_value = 120.0;
    check("a correctly-seen empty shelf is fully trusted",
          vision::confidence(empty_shelf) == 100);
}

void test_hue_arithmetic()
{
    suite("hue arithmetic");

    // OpenCV's hue is 0-179 and wraps. Red straddles the join, so this is not
    // a corner case for this project - it is Coke.
    check("distance goes the short way round the wrap",
          vision::hue_distance(178, 2) == 4.0);
    check("distance is symmetric", vision::hue_distance(2, 178) == 4.0);
    check("distance within the range is plain subtraction",
          vision::hue_distance(40, 55) == 15.0);
    check("opposite hues are 90 apart, the maximum possible",
          vision::hue_distance(0, 90) == 90.0);

    check("the mean of two ordinary hues is between them",
          vision::circular_hue_mean(40, 50) == 45.0);

    // The arithmetic mean of 178 and 2 is 90 - cyan, the opposite side of the
    // wheel from the red that was actually sampled. Saving that as Coke's
    // identity would make Coke unrecognisable AND make it claim Mountain Dew.
    const double wrapped = vision::circular_hue_mean(178, 2);
    check("the mean across the wrap is red, not the cyan an average gives",
          wrapped >= 179.0 || wrapped <= 1.0);

    check("the mean across the wrap is symmetric",
          vision::circular_hue_mean(2, 178) == vision::circular_hue_mean(178, 2));

    // --- The guard that would have prevented the wrecked Solo band ---
    check("two clicks on one can agree", vision::samples_agree(27, 30));
    check("the lit and shadowed sides of one can still agree",
          vision::samples_agree(46, 48));
    check("a Coke and a Fanta do NOT agree", !vision::samples_agree(0, 40));
    check("a Solo and a Mountain Dew do NOT agree",
          !vision::samples_agree(28, 47));
    check("agreement respects the wrap, so one red can is not two drinks",
          vision::samples_agree(178, 3));
}

void test_sample_set()
{
    suite("sampling a can");

    std::string error;

    // --- An ordinary patch off one can ---
    vision::SampleSet set;
    for (int i = 0; i < 20; i++) {
        set.add(9 + (i % 3), 240 + (i % 10), 200 + (i % 20));
    }
    vision::Brand brand;
    check("a patch produces a colour", set.to_brand(brand, &error));
    check("the hue lands inside the sampled range",
          brand.hue >= 9 && brand.hue <= 11);
    check("saturation and value land inside their sampled ranges",
          brand.saturation >= 240 && brand.saturation <= 249 &&
          brand.value >= 200 && brand.value <= 219);

    // --- One bad pixel must not move the answer ---
    //
    // The whole reason a patch beats a click. A glare speck reads as a
    // completely different hue, and under the old two-pixel scheme it WAS the
    // measurement if you happened to click on it.
    vision::SampleSet with_outlier;
    for (int i = 0; i < 40; i++) with_outlier.add(9, 245, 210);
    with_outlier.add(150, 40, 250);
    vision::Brand robust;
    check("a single outlying pixel does not move the colour",
          with_outlier.to_brand(robust, &error) && robust.hue == 9);

    // --- Hue 0, which the band format could never store ---
    vision::SampleSet red;
    for (int i = 0; i < 10; i++) red.add(0, 250, 190);
    red.add(179, 250, 190);
    red.add(1, 250, 190);
    vision::Brand coke;
    check("a can sitting on the hue wrap averages to the wrap, not away "
          "from it",
          red.to_brand(coke, &error) && (coke.hue <= 1 || coke.hue >= 178));

    // --- Two different drinks in one sample set ---
    //
    // What clicking on a Coke and then a Mountain Dew looks like from in here.
    // Refused, because the average of two drinks is a colour that then claims
    // both of them.
    vision::SampleSet two_drinks;
    for (int i = 0; i < 20; i++) two_drinks.add(0, 250, 190);
    for (int i = 0; i < 20; i++) two_drinks.add(45, 220, 150);
    vision::Brand nonsense;
    error.clear();
    check("samples spanning two drinks are refused",
          !two_drinks.to_brand(nonsense, &error) && !error.empty());

    vision::SampleSet nothing;
    error.clear();
    check("an empty sample set is refused",
          !nothing.to_brand(nonsense, &error) && !error.empty());

    // --- The spread figure people are asked to read ---
    vision::SampleSet tight;
    for (int i = 0; i < 30; i++) tight.add(27 + (i % 2), 250, 200);
    check("a tight patch reports a small spread", tight.hue_spread() <= 2.0);
    check("clearing empties it",
          (tight.clear(), tight.empty() && tight.size() == 0));
}

} // namespace

int main()
{
    printf("\n=== picapture vision_config tests ===\n");

    test_defaults();
    test_hue_arithmetic();
    test_sample_set();
    test_round_trip();
    test_partial_and_missing();
    test_refusals();
    test_validation();
    test_normalise();
    test_confidence();

    printf("\n=========================================\n");
    printf("%d checks, %d failed\n", checks, failures);
    printf("%s\n", failures == 0 ? "ALL TESTS PASSED" : "FAILURES");
    printf("=========================================\n");
    return failures == 0 ? 0 : 1;
}
