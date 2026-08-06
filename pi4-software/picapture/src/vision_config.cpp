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

/// `name | wire_key | lowH,highH,lowS,highS,lowV,highV`
bool parse_brand(const std::string &value, Brand &out, std::string *error)
{
    const std::vector<std::string> fields = split(value, '|');
    if (fields.size() != 3) {
        if (error) *error = "a brand needs 'name | wire_key | six HSV numbers'";
        return false;
    }

    const std::vector<std::string> hsv = split(fields[2], ',');
    if (hsv.size() != 6) {
        if (error) *error = "a brand needs exactly six HSV numbers";
        return false;
    }

    Brand brand;
    brand.name = fields[0];
    brand.wire_key = fields[1];
    int *targets[6] = {&brand.lowH, &brand.highH, &brand.lowS,
                       &brand.highS, &brand.lowV, &brand.highV};
    for (size_t i = 0; i < 6; i++) {
        if (!parse_int(hsv[i], *targets[i])) {
            if (error) *error = "'" + hsv[i] + "' is not a whole number";
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
    config.brands = {
        // name            wire_key    H low/high   S low/high   V low/high
        {"Coke",           "coke",      0,   6,     220, 255,    160, 220},
        {"Fanta",          "fanta",     3,  17,     236, 255,    130, 165},
        {"Mountain Dew",   "mtndew",   41,  55,     133, 255,     66, 103},
        {"Solo",           "solo",     21,  34,     231, 255,     71, 103},
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
        } else if (key == "max_brand_dist") {
            ok = parse_double(value, config.max_brand_dist);
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
            "\n";

    for (const Brand &brand : config.brands) {
        file << "brand = " << brand.name << " | " << brand.wire_key << " | "
             << brand.lowH << "," << brand.highH << ","
             << brand.lowS << "," << brand.highS << ","
             << brand.lowV << "," << brand.highV << "\n";
    }

    file << "\n# Classification\n"
         << "hue_weight = " << config.hue_weight << "\n"
         << "max_brand_dist = " << config.max_brand_dist << "\n"
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
         << "period_ms = " << config.period_ms << "\n";

    file.flush();
    if (!file) {
        if (error) *error = "failed while writing " + path;
        return false;
    }
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
        if (brand.lowH < 0 || brand.highH > 179 || brand.lowH > brand.highH) {
            return fail(where + ": hue must be 0-179 with low <= high "
                                "(OpenCV halves the angle to fit a byte)");
        }
        if (brand.lowS < 0 || brand.highS > 255 || brand.lowS > brand.highS) {
            return fail(where + ": saturation must be 0-255 with low <= high");
        }
        if (brand.lowV < 0 || brand.highV > 255 || brand.lowV > brand.highV) {
            return fail(where + ": value must be 0-255 with low <= high");
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
    if (config.max_brand_dist <= 0.0) {
        return fail("max_brand_dist must be positive, or nothing is ever "
                    "classified");
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
