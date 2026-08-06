#include "vision_config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace vision {

namespace {

/// Trim ASCII whitespace from both ends. Written out rather than pulled from a
/// library because this file deliberately has no dependencies at all.
std::string trim(const std::string &text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace((unsigned char)text[begin])) begin++;
    while (end > begin && std::isspace((unsigned char)text[end - 1])) end--;
    return text.substr(begin, end - begin);
}

/// Split on `sep`, keeping empty fields. Empty fields matter: they are how a
/// malformed brand line is detected rather than silently absorbed.
std::vector<std::string> split(const std::string &text, char sep)
{
    std::vector<std::string> parts;
    std::string item;
    std::istringstream stream(text);
    while (std::getline(stream, item, sep)) {
        parts.push_back(trim(item));
    }
    return parts;
}

/// Parse a whole-number field. Returns false rather than throwing, and rather
/// than std::atoi's silent zero — "opnе_kernel = 3" with a Cyrillic е should
/// report a bad key, not turn morphology off and say nothing.
bool parse_int(const std::string &text, int &out)
{
    if (text.empty()) return false;
    try {
        size_t consumed = 0;
        const int value = std::stoi(text, &consumed);
        if (consumed != text.size()) return false;
        out = value;
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

bool parse_double(const std::string &text, double &out)
{
    if (text.empty()) return false;
    try {
        size_t consumed = 0;
        const double value = std::stod(text, &consumed);
        if (consumed != text.size()) return false;
        out = value;
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

bool parse_bool(const std::string &text, bool &out)
{
    std::string lowered;
    for (char c : text) lowered += (char)std::tolower((unsigned char)c);
    if (lowered == "true" || lowered == "yes" || lowered == "1") {
        out = true;
        return true;
    }
    if (lowered == "false" || lowered == "no" || lowered == "0") {
        out = false;
        return true;
    }
    return false;
}

/// `name | wire_key | hue,saturation,value [| can_area]`
/// or the older `name | wire_key | lowH,highH,lowS,highS,lowV,highV [| can_area]`
///
/// The fourth field is optional so that a config written before per-brand can
/// areas existed still loads, with the area defaulting to "not calibrated".
/// Refusing those files would mean everybody's tuning had to be redone to gain
/// a feature that is itself opt-in.
bool parse_brand(const std::string &value, Brand &out, std::string *error)
{
    const std::vector<std::string> fields = split(value, '|');
    if (fields.size() != 3 && fields.size() != 4) {
        if (error) {
            *error = "a brand needs 'name | wire_key | six HSV numbers' and "
                     "optionally '| can_area'";
        }
        return false;
    }

    // Three numbers is the colour itself. Six is a low/high band written before
    // the format changed, and is converted rather than refused — the old files
    // hold real tuning that somebody stood at a fridge to produce, and the
    // information needed is in them.
    const std::vector<std::string> hsv = split(fields[2], ',');
    if (hsv.size() != 3 && hsv.size() != 6) {
        if (error) {
            *error = "a brand needs three HSV numbers (its colour), or six "
                     "(an old low/high band)";
        }
        return false;
    }

    std::vector<int> numbers(hsv.size(), 0);
    for (size_t i = 0; i < hsv.size(); i++) {
        if (!parse_int(hsv[i], numbers[i])) {
            if (error) *error = "'" + hsv[i] + "' is not a whole number";
            return false;
        }
    }

    Brand brand;
    brand.name = fields[0];
    brand.wire_key = fields[1];

    if (numbers.size() == 3) {
        brand.hue = numbers[0];
        brand.saturation = numbers[1];
        brand.value = numbers[2];
    } else {
        // The midpoint is what the classifier always used, so converting loses
        // nothing that was ever read. Circular for hue, because a legacy band
        // could not represent a colour at hue 0 and clamped instead — the
        // conversion cannot recover what that clamping already threw away, but
        // it must not add more.
        brand.hue = (int)std::lround(circular_hue_mean(numbers[0], numbers[1]));
        brand.saturation = (numbers[2] + numbers[3]) / 2;
        brand.value = (numbers[4] + numbers[5]) / 2;
    }

    if (fields.size() == 4 && !fields[3].empty()) {
        if (!parse_int(fields[3], brand.can_area)) {
            if (error) *error = "'" + fields[3] + "' is not a whole number";
            return false;
        }
    }

    if (brand.name.empty() || brand.wire_key.empty()) {
        if (error) *error = "a brand needs both a name and a wire key";
        return false;
    }

    out = brand;
    return true;
}

} // namespace

Config defaults()
{
    Config config;

    // Sampled from cans_test/ under one set of lights. The order is
    // catalogue::Can's order and must stay that way; the wire keys are
    // catalogue::wire_key() and must match it character for character.
    // ORDER IS catalogue::Can's ORDER and must stay that way — counts are
    // reported positionally. Note it is NOT hue order: Solo sits between Fanta
    // and Mountain Dew on the colour wheel but last but one in the catalogue.
    //
    // Values measured off the fridge camera with the exposure settled. Coke at
    // hue 0 is exactly the case the old low/high band could not store.
    config.brands = {
        // name            wire_key      H    S    V   can area
        {"Coke",           "coke",       0, 247, 190,  0},
        {"Fanta",          "fanta",      9, 247, 220,  0},
        {"Mountain Dew",   "mtndew",    44, 220, 150,  0},
        {"Solo",           "solo",      27, 247, 200,  0},
    };

    return config;
}

bool load(const std::string &path, Config &out, std::string *error)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;      // never tuned on this machine; defaults stand
    }

    Config config = defaults();

    // Cleared, not appended to. A file that lists brands REPLACES the built-in
    // list rather than adding to it — otherwise correcting a colour would leave
    // the old one in place and every count would be claimed twice.
    bool brands_seen = false;

    std::string line;
    int line_number = 0;
    while (std::getline(file, line)) {
        line_number++;

        const size_t comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        line = trim(line);
        if (line.empty()) continue;

        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            if (error) {
                *error = path + ":" + std::to_string(line_number) +
                         ": expected 'key = value'";
            }
            return false;
        }

        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        std::string detail;
        bool ok = true;

        if (key == "brand") {
            if (!brands_seen) {
                config.brands.clear();
                brands_seen = true;
            }
            Brand brand;
            ok = parse_brand(value, brand, &detail);
            if (ok) config.brands.push_back(brand);
        } else if (key == "hue_weight") {
            ok = parse_double(value, config.hue_weight);
        } else if (key == "sat_weight") {
            ok = parse_double(value, config.sat_weight);
        } else if (key == "val_weight") {
            ok = parse_double(value, config.val_weight);
        } else if (key == "max_brand_dist") {
            ok = parse_double(value, config.max_brand_dist);
        } else if (key == "min_saturation") {
            ok = parse_int(value, config.min_saturation);
        } else if (key == "min_value") {
            ok = parse_int(value, config.min_value);
        } else if (key == "libcamerasrc_extra") {
            // Taken verbatim, including spaces: it is a fragment of a GStreamer
            // pipeline, not a scalar. Empty is valid and means "add nothing".
            config.libcamerasrc_extra = value;
        } else if (key == "open_kernel") {
            ok = parse_int(value, config.open_kernel);
        } else if (key == "close_kernel") {
            ok = parse_int(value, config.close_kernel);
        } else if (key == "contour_min_area") {
            ok = parse_int(value, config.contour_min_area);
        } else if (key == "contour_max_area") {
            ok = parse_int(value, config.contour_max_area);
        } else if (key == "flat_field_blur") {
            ok = parse_int(value, config.flat_field_blur);
        } else if (key == "capture_width") {
            ok = parse_int(value, config.capture_width);
        } else if (key == "capture_height") {
            ok = parse_int(value, config.capture_height);
        } else if (key == "process_width") {
            ok = parse_int(value, config.process_width);
        } else if (key == "process_height") {
            ok = parse_int(value, config.process_height);
        } else if (key == "rotate_180") {
            ok = parse_bool(value, config.rotate_180);
        } else if (key == "period_ms") {
            ok = parse_int(value, config.period_ms);
        } else {
            // Refused rather than ignored. A typo in a key is exactly the bug
            // this file makes easy to introduce, and silently running with the
            // default is how somebody spends an afternoon wondering why their
            // change did nothing.
            if (error) {
                *error = path + ":" + std::to_string(line_number) +
                         ": unknown setting '" + key + "'";
            }
            return false;
        }

        if (!ok) {
            if (error) {
                *error = path + ":" + std::to_string(line_number) + ": " +
                         (detail.empty() ? "could not read '" + value + "'"
                                         : detail);
            }
            return false;
        }
    }

    out = config;
    return true;
}

bool save(const std::string &path, const Config &config, std::string *error)
{
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        if (error) *error = "could not open " + path + " for writing";
        return false;
    }

    file << "# picapture tuning, written by the debug UI.\n"
            "#\n"
            "# Anything absent falls back to the compiled-in default, so it is\n"
            "# safe to delete a line you have not deliberately changed - and\n"
            "# safer than keeping a stale copy of a value somebody has since\n"
            "# improved in the source.\n"
            "#\n"
            "# Listing ANY brand replaces the whole built-in list, so all four\n"
            "# have to appear together. Their order is the order counts are\n"
            "# reported in, and it must match catalogue::Can on the RP2040.\n"
            "#\n"
            "#   name | wire key | hue,saturation,value | can area\n"
            "#\n"
            "# The colour is a single point, not a range: a pixel goes to\n"
            "# whichever drink's colour it is nearest. Hue is 0-179, not\n"
            "# 0-359 - OpenCV halves it to fit a byte.\n"
            "#\n"
            "# The can area is the size of ONE can in pixels, and is what lets\n"
            "# two touching cans be counted as two rather than as one blob.\n"
            "# Zero means uncalibrated: press 'c' with one can in view.\n"
            "\n";

    for (const Brand &brand : config.brands) {
        file << "brand = " << brand.name << " | " << brand.wire_key << " | "
             << brand.hue << "," << brand.saturation << "," << brand.value
             << " | " << brand.can_area << "\n";
    }

    file << "\n# Classification. Hue identifies the drink; saturation and value\n"
            "# mostly describe the lighting, so they are weighted well below it.\n"
         << "hue_weight = " << config.hue_weight << "\n"
         << "sat_weight = " << config.sat_weight << "\n"
         << "val_weight = " << config.val_weight << "\n"
         << "max_brand_dist = " << config.max_brand_dist << "\n"
         << "\n# What counts as a drink at all, rather than shelf or shadow.\n"
            "# Global on purpose: these must never decide WHICH drink.\n"
         << "min_saturation = " << config.min_saturation << "\n"
         << "min_value = " << config.min_value << "\n"
         << "\n# Mask cleanup\n"
         << "open_kernel = " << config.open_kernel << "\n"
         << "close_kernel = " << config.close_kernel << "\n"
         << "contour_min_area = " << config.contour_min_area << "\n"
         << "contour_max_area = " << config.contour_max_area << "\n"
         << "flat_field_blur = " << config.flat_field_blur << "\n"
         << "\n# Camera\n"
         << "capture_width = " << config.capture_width << "\n"
         << "capture_height = " << config.capture_height << "\n"
         << "process_width = " << config.process_width << "\n"
         << "process_height = " << config.process_height << "\n"
         << "rotate_180 = " << (config.rotate_180 ? "true" : "false") << "\n"
         << "period_ms = " << config.period_ms << "\n"
         << "# Appended to libcamerasrc verbatim. Chiefly for locking exposure\n"
            "# and white balance - see `gst-inspect-1.0 libcamerasrc`.\n"
         << "libcamerasrc_extra = " << config.libcamerasrc_extra << "\n";

    file.flush();
    if (!file) {
        if (error) *error = "failed while writing " + path;
        return false;
    }
    return true;
}

double circular_hue_mean(double a, double b)
{
    double diff = b - a;
    if (diff > 90.0) diff -= 180.0;
    if (diff < -90.0) diff += 180.0;

    double mean = a + diff / 2.0;
    if (mean < 0.0) mean += 180.0;
    if (mean >= 180.0) mean -= 180.0;
    return mean;
}

bool samples_agree(int hue_a, int hue_b)
{
    return hue_distance(hue_a, hue_b) <= Config::MAX_SAMPLE_HUE_SPREAD;
}

void SampleSet::add(int hue, int saturation, int value)
{
    hues_.push_back(hue);
    saturations_.push_back(saturation);
    values_.push_back(value);
}

void SampleSet::clear()
{
    hues_.clear();
    saturations_.clear();
    values_.clear();
}

namespace {

constexpr double PI = 3.14159265358979323846;

/// Where `hue` sits relative to `origin`, as a signed offset in [-90, 90].
double signed_hue_offset(double hue, double origin)
{
    double offset = hue - origin;
    while (offset > 90.0) offset -= 180.0;
    while (offset < -90.0) offset += 180.0;
    return offset;
}

} // namespace

double SampleSet::centre_hue() const
{
    if (hues_.empty()) return 0.0;

    // FIRST PASS: the circular mean, computed as ANGLES rather than numbers.
    // Hue is a circle that OpenCV represents as 0-179, so red sits at both
    // ends of the range and the arithmetic mean of 178 and 2 is 90 — cyan.
    // Summing unit vectors and taking the direction of the total has no such
    // seam. The doubling is because 0-179 covers a full turn, so one step of
    // hue is two degrees.
    double sum_sin = 0.0;
    double sum_cos = 0.0;
    for (int hue : hues_) {
        const double angle = hue * 2.0 * PI / 180.0;
        sum_sin += std::sin(angle);
        sum_cos += std::cos(angle);
    }
    double rough = std::atan2(sum_sin, sum_cos) * 180.0 / (2.0 * PI);
    if (rough < 0.0) rough += 180.0;

    // SECOND PASS: the median offset from that estimate.
    //
    // A mean is not enough on its own. One glare speck reading as a completely
    // different hue drags a mean of forty pixels by a whole point, and the
    // entire reason for sampling a patch is that one bad pixel should count
    // for nothing. Taking the median of the offsets — rather than of the hues
    // themselves — keeps the wrap handled by the first pass while making the
    // answer indifferent to outliers.
    std::vector<double> offsets;
    offsets.reserve(hues_.size());
    for (int hue : hues_) {
        offsets.push_back(signed_hue_offset(hue, rough));
    }
    std::sort(offsets.begin(), offsets.end());

    double centre = rough + offsets[offsets.size() / 2];
    if (centre < 0.0) centre += 180.0;
    if (centre >= 180.0) centre -= 180.0;
    return centre;
}

double SampleSet::hue_spread() const
{
    if (hues_.empty()) return 0.0;

    const double centre = centre_hue();
    std::vector<double> distances;
    distances.reserve(hues_.size());
    for (int hue : hues_) {
        distances.push_back(hue_distance(hue, centre));
    }
    std::sort(distances.begin(), distances.end());

    // The 90th percentile rather than the maximum. A patch clicked near the
    // edge of a can catches a few background pixels however carefully it is
    // placed, and letting the single worst of them define the spread would
    // make every sample look like two drinks.
    const size_t index = (size_t)((distances.size() - 1) * 0.9);
    return distances[index];
}

bool SampleSet::to_brand(Brand &brand, std::string *error) const
{
    if (hues_.empty()) {
        if (error) *error = "no pixels were sampled";
        return false;
    }

    const double spread = hue_spread();
    if (spread > Config::MAX_SAMPLE_HUE_SPREAD) {
        if (error) {
            *error = "the sampled pixels span " + std::to_string((int)spread) +
                     " hue, which is too much for one can - check the drink "
                     "named on the camera window and click on ONE can";
        }
        return false;
    }

    // Medians, not means, for saturation and value: a glare speck is a genuine
    // outlier at the top of the range and a shadowed crevice at the bottom,
    // and neither should move the answer.
    std::vector<int> saturations = saturations_;
    std::vector<int> values = values_;
    std::sort(saturations.begin(), saturations.end());
    std::sort(values.begin(), values.end());

    brand.hue = (int)std::lround(centre_hue());
    if (brand.hue > 179) brand.hue = 179;
    if (brand.hue < 0) brand.hue = 0;
    brand.saturation = saturations[saturations.size() / 2];
    brand.value = values[values.size() / 2];
    return true;
}

bool normalise(Config &config, std::string *note)
{
    std::ostringstream changes;
    bool moved = false;

    const struct { const char *name; int *value; } kernels[] = {
        {"open_kernel", &config.open_kernel},
        {"close_kernel", &config.close_kernel},
    };

    for (const auto &kernel : kernels) {
        int &value = *kernel.value;
        if (value > 0 && value % 2 == 0) {
            if (moved) changes << "; ";
            changes << kernel.name << " " << value << " -> " << (value + 1)
                    << " (a kernel needs a centre pixel, so it must be odd)";
            value += 1;
            moved = true;
        }
    }

    if (moved && note != nullptr) *note = changes.str();
    return moved;
}

bool validate(const Config &config, std::string *error)
{
    auto fail = [&](const std::string &message) {
        if (error) *error = message;
        return false;
    };

    if (config.brands.empty()) {
        return fail("no brands configured - nothing could ever be detected");
    }

    for (size_t i = 0; i < config.brands.size(); i++) {
        const Brand &brand = config.brands[i];
        const std::string where = "brand '" + brand.name + "'";

        // Lower case and space-free for the same reason catalogue.h insists on
        // it: this key ends up in a space-separated key=value payload, and a
        // space in it does not fail loudly - it truncates the field and loses
        // every drink after it.
        for (char c : brand.wire_key) {
            if (std::isspace((unsigned char)c) || c == '=' || c == ',' ||
                c == '|' || std::isupper((unsigned char)c)) {
                return fail(where + ": wire key '" + brand.wire_key +
                            "' must be lower case with no spaces or separators");
            }
        }

        for (size_t j = i + 1; j < config.brands.size(); j++) {
            if (config.brands[j].wire_key == brand.wire_key) {
                return fail("two brands share the wire key '" +
                            brand.wire_key + "'");
            }
        }

        // OpenCV's hue is 0-179, not 0-359: it halves the angle to fit a byte.
        // Copying a value out of a colour picker without halving it is the
        // single easiest mistake to make here, and 0-179 catches it.
        if (brand.hue < 0 || brand.hue > 179) {
            return fail(where + ": hue must be 0-179 (OpenCV halves the angle "
                                "to fit a byte, so a value from a normal "
                                "colour picker needs dividing by two)");
        }
        if (brand.saturation < 0 || brand.saturation > 255) {
            return fail(where + ": saturation must be 0-255");
        }
        if (brand.value < 0 || brand.value > 255) {
            return fail(where + ": value must be 0-255");
        }

        // A colour the floors would reject can never be matched: every pixel
        // near it is background before any brand is considered, so this drink
        // would silently never be detected.
        if (brand.saturation < config.min_saturation ||
            brand.value < config.min_value) {
            return fail(where + ": its colour is below min_saturation (" +
                        std::to_string(config.min_saturation) +
                        ") or min_value (" + std::to_string(config.min_value) +
                        "), so no pixel could ever be matched to it");
        }

        if (brand.can_area < 0) {
            return fail(where + ": can_area cannot be negative");
        }

        // A can smaller than the smallest blob worth looking at is a
        // contradiction, and a nasty one: every accepted blob would then
        // divide to two or more cans and the shelf would appear to double.
        if (brand.can_area > 0 && brand.can_area < config.contour_min_area) {
            return fail(where + ": can_area (" +
                        std::to_string(brand.can_area) +
                        ") is below contour_min_area (" +
                        std::to_string(config.contour_min_area) +
                        "), so every blob would count as two or more cans");
        }
    }

    // getStructuringElement is happy with an even kernel but it has no centre
    // pixel, so the mask shifts by half a pixel each pass. Refused rather than
    // rounded, because a silently altered kernel is a value that does not match
    // what the file says.
    if (config.open_kernel < 0 || config.open_kernel > Config::MAX_KERNEL ||
        (config.open_kernel > 0 && config.open_kernel % 2 == 0)) {
        return fail("open_kernel must be 0, or an odd number up to " +
                    std::to_string(Config::MAX_KERNEL));
    }
    if (config.close_kernel < 0 || config.close_kernel > Config::MAX_KERNEL ||
        (config.close_kernel > 0 && config.close_kernel % 2 == 0)) {
        return fail("close_kernel must be 0, or an odd number up to " +
                    std::to_string(Config::MAX_KERNEL));
    }

    if (config.contour_min_area < 0 ||
        config.contour_min_area >= config.contour_max_area) {
        return fail("contour_min_area must be below contour_max_area");
    }
    if (config.contour_max_area > Config::MAX_AREA) {
        return fail("contour_max_area is above the slider's range (" +
                    std::to_string(Config::MAX_AREA) + ")");
    }

    // GaussianBlur requires an odd kernel. Same argument as the morphology
    // kernels: refuse rather than quietly use a different number.
    if (config.flat_field_blur < 0 ||
        (config.flat_field_blur > 0 && config.flat_field_blur % 2 == 0)) {
        return fail("flat_field_blur must be 0 or an odd number");
    }

    if (config.hue_weight <= 0.0) {
        return fail("hue_weight must be positive - hue is the only thing that "
                    "reliably separates these drinks");
    }
    if (config.sat_weight < 0.0 || config.val_weight < 0.0) {
        return fail("sat_weight and val_weight cannot be negative - a negative "
                    "weight rewards being the wrong colour");
    }

    // The whole design rests on hue deciding and the other two only nudging.
    // A config that inverts that is not a tuning choice, it is the bug this
    // was written to fix: value out-voting hue is what made a Coke in shadow
    // classify as a Fanta.
    if (config.sat_weight >= config.hue_weight ||
        config.val_weight >= config.hue_weight) {
        return fail("hue_weight must exceed both sat_weight and val_weight - "
                    "saturation and value move with the lighting, so letting "
                    "either outweigh hue makes a can change drink when a "
                    "shadow crosses it");
    }

    if (config.max_brand_dist <= 0.0) {
        return fail("max_brand_dist must be positive, or nothing is ever "
                    "classified");
    }

    if (config.min_saturation < 0 || config.min_saturation > 255 ||
        config.min_value < 0 || config.min_value > 255) {
        return fail("min_saturation and min_value must be 0-255");
    }

    if (config.capture_width <= 0 || config.capture_height <= 0 ||
        config.process_width <= 0 || config.process_height <= 0) {
        return fail("camera dimensions must all be positive");
    }
    if (config.process_width > config.capture_width ||
        config.process_height > config.capture_height) {
        return fail("processing resolution cannot exceed the capture "
                    "resolution - upscaling invents detail rather than "
                    "revealing it");
    }

    // The board faults if a scan is unanswered for RECOUNT_TIMEOUT_MS (8 s),
    // and the answer has to wait for several agreeing frames to settle. A
    // period anywhere near that budget leaves no room to settle in.
    if (config.period_ms < 20 || config.period_ms > 2000) {
        return fail("period_ms must be between 20 and 2000 - the board's "
                    "recount budget is 8 s and a settled answer needs several "
                    "frames inside it");
    }

    return true;
}

} // namespace vision
