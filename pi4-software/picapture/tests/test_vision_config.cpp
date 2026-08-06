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
    original.brands[2].lowH = 44;
    original.brands[2].highH = 52;

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
                    a.lowH == b.lowH && a.highH == b.highH &&
                    a.lowS == b.lowS && a.highS == b.highS &&
                    a.lowV == b.lowV && a.highV == b.highV;
    }
    check("every brand survives, name and band intact", brands_ok);

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
         [](vision::Config &c) { c.brands[0].highH = 200; }},
        {"an inverted band is rejected",
         [](vision::Config &c) { c.brands[0].lowS = 200; c.brands[0].highS = 100; }},
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

    // --- Two clicks becoming a band ---
    vision::Brand brand;
    vision::apply_sample(brand, 10, 240, 200, 12, 250, 180);
    check("an ordinary pair produces a band containing both hues",
          brand.lowH <= 10 && brand.highH >= 12);
    check("the band is the right way round",
          brand.lowH <= brand.highH && brand.lowS <= brand.highS &&
          brand.lowV <= brand.highV);
    check("saturation and value span both samples",
          brand.lowS <= 240 && brand.highS >= 250 &&
          brand.lowV <= 180 && brand.highV >= 200);

    // The wrap case has to produce a VALID band, not lowH=172/highH=8. That
    // is not merely wrong, it is a band validate() refuses - so the symptom
    // was a save that failed for a reason nowhere near the actual click.
    vision::Brand wrapping;
    vision::apply_sample(wrapping, 178, 250, 190, 2, 250, 170);
    check("a pair straddling the wrap still produces a valid band",
          wrapping.lowH <= wrapping.highH);
    check("...and it stays inside OpenCV's 0-179 hue range",
          wrapping.lowH >= 0 && wrapping.highH <= 179);

    vision::Config config = vision::defaults();
    config.brands[0] = wrapping;
    std::string error;
    check("...and a config built from it validates",
          vision::validate(config, &error));
    if (!error.empty()) printf("        %s\n", error.c_str());

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

} // namespace

int main()
{
    printf("\n=== picapture vision_config tests ===\n");

    test_defaults();
    test_hue_arithmetic();
    test_round_trip();
    test_partial_and_missing();
    test_refusals();
    test_validation();
    test_normalise();

    printf("\n=========================================\n");
    printf("%d checks, %d failed\n", checks, failures);
    printf("%s\n", failures == 0 ? "ALL TESTS PASSED" : "FAILURES");
    printf("=========================================\n");
    return failures == 0 ? 0 : 1;
}
