// picapture: what is on the shelf, worked out from a camera.
//
// Reads the Pi camera, decides how many of each drink it can see, and prints
// one line per look:
//
//     coke:5,fanta:4,mtndew:5,solo:3;
//
// It talks to nobody. `fridged` runs this as a subprocess and reads its
// standard output, which is what keeps the serial link owned by exactly one
// process. Anything printed here that is not a count line goes to stderr and is
// diagnostic; stdout carries counts and nothing else.
//
// REQUIRES OPENCV 4.x. OpenCV 5.0.0 dropped the Moments class this depends on.
//
// HOW A FRAME BECOMES A COUNT
// ---------------------------
//   1. capture, downsample
//   2. flat-field correct   divide out large-scale lighting, so a shadowed can
//                           and a lit can are still the same colour
//   3. convert to HSV
//   4. classify             assign every pixel to its NEAREST brand, once
//   5. per brand            clean up the mask, find contours, count them
//
// Step 4 is the one worth understanding. The obvious approach — one inRange()
// per brand — is what this did first, and it does not work here: Coke, Fanta,
// Mountain Dew and Solo occupy adjacent hue bands, so glare, JPEG blocking and
// anti-aliased edges routinely satisfy two or three brands at once, and a
// single can picks up several labels simultaneously. Nearest-centre gives every
// pixel exactly one owner, so two brands can no longer claim the same region.
//
// EVERY THRESHOLD LIVES IN vision_config.h. There are no bare numbers below.

#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "vision_config.h"

namespace {

// --- How much of a window this run puts up ---------------------------------

enum class Mode {
    Headless,    ///< No GUI at all. What the deployed service runs.
    DebugCamera, ///< The annotated camera view.
    DebugAll,    ///< ...plus the intermediate stages and the tuning sliders.
};

/// Where tuning is read from and written back to, relative to the working
/// directory. The service runs with its working directory set to the picapture
/// folder, so this sits beside the binary's parent, not inside build/.
constexpr const char *CONFIG_PATH = "picapture.conf";

/// A pixel belonging to no brand.
constexpr signed char BACKGROUND_LABEL = -1;

/// Window titles. Named constants because a typo in one of these produces a
/// second, empty window rather than a compile error.
constexpr const char *WIN_CAMERA = "picapture - camera";
constexpr const char *WIN_MASK = "picapture - classified";
constexpr const char *WIN_MORPH = "picapture - after cleanup";
constexpr const char *WIN_FLAT = "picapture - flat-field corrected";
constexpr const char *WIN_CONTROL = "picapture - tuning";

// --- The pipeline's working images -----------------------------------------

struct Frames {
    cv::Mat original;       ///< As captured, then drawn on for the debug view
    cv::Mat flattened;      ///< After flat-field correction
    cv::Mat hsv;
    cv::Mat classification; ///< Per-pixel brand index, or BACKGROUND_LABEL
    cv::Mat mask;           ///< The current brand's pixels
    cv::Mat morphology;     ///< ...after opening and closing
};

struct Detection {
    cv::Rect bbox;
    cv::Point2f centroid;
    double area;
};

/// A brand's HSV centre, precomputed from its configured box.
///
/// No per-brand floors any more. They used to live here and they caused the
/// bug they were meant to prevent: Coke floored value at 160 and Fanta at 130,
/// so a Coke can in shadow was refused by Coke and accepted by Fanta. Whether a
/// pixel is a drink is now a global question; which drink is settled purely by
/// distance.
struct BrandCentre {
    double h, s, v;
};

/// Click-to-sample: two clicks on a can set that drink's colour band.
struct SampleState {
    const cv::Mat *frame = nullptr;  ///< The image clicks are read from
    int brand_index = 0;             ///< Which brand the clicks tune
    int clicks = 0;
    cv::Vec3b first{};
    cv::Vec3b second{};
    bool pair_ready = false;

    // For the per-click diagnostic. Pointers rather than copies so the readout
    // reflects tuning as it is edited, not as it was when the window opened.
    const vision::Config *config = nullptr;
    const std::vector<BrandCentre> *centres = nullptr;
};

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

std::vector<BrandCentre> build_centres(const vision::Config &config)
{
    std::vector<BrandCentre> centres;
    centres.reserve(config.brands.size());
    for (const vision::Brand &brand : config.brands) {
        centres.push_back({
            (brand.lowH + brand.highH) / 2.0,
            (brand.lowS + brand.highS) / 2.0,
            (brand.lowV + brand.highV) / 2.0,
        });
    }
    return centres;
}

using vision::hue_distance;

/// How far one pixel is from one brand.
///
/// Factored out so the classifier and the click diagnostic can never disagree
/// about it. A tuning readout that computed the distance slightly differently
/// from the classifier would be worse than no readout at all.
inline double brand_distance(const cv::Vec3b &px, const BrandCentre &centre,
                             const vision::Config &config)
{
    const double dh = hue_distance(px[0], centre.h) * config.hue_weight;
    const double ds = ((double)px[1] - centre.s) * config.sat_weight;
    const double dv = ((double)px[2] - centre.v) * config.val_weight;
    return std::sqrt(dh * dh + ds * ds + dv * dv);
}

/// Is this pixel a drink at all?
///
/// Global, and deliberately answered before any brand is considered. Asking it
/// per brand is what let a shadow rename a can: two brands with different value
/// floors meant a dimmed Coke failed Coke's floor, passed Fanta's, and became a
/// Fanta rather than becoming unknown.
inline bool is_candidate(const cv::Vec3b &px, const vision::Config &config)
{
    return px[1] >= config.min_saturation && px[2] >= config.min_value;
}

/// Assign every pixel to its nearest brand, or to the background.
///
/// One pass covering all brands rather than one pass per brand: cheaper, and —
/// the reason it was written this way — it is what guarantees a pixel has a
/// single owner.
cv::Mat classify(const cv::Mat &hsv, const std::vector<BrandCentre> &centres,
                 const vision::Config &config)
{
    cv::Mat labels(hsv.size(), CV_8SC1, cv::Scalar(BACKGROUND_LABEL));

    for (int y = 0; y < hsv.rows; ++y) {
        const cv::Vec3b *row = hsv.ptr<cv::Vec3b>(y);
        signed char *out = labels.ptr<signed char>(y);

        for (int x = 0; x < hsv.cols; ++x) {
            const cv::Vec3b &px = row[x];
            if (!is_candidate(px, config)) continue;   // shelf, shadow, glare

            double best = std::numeric_limits<double>::max();
            int best_label = BACKGROUND_LABEL;

            for (size_t i = 0; i < centres.size(); ++i) {
                const double dist = brand_distance(px, centres[i], config);
                if (dist < best) {
                    best = dist;
                    best_label = (int)i;
                }
            }

            if (best <= config.max_brand_dist) {
                out[x] = (signed char)best_label;
            }
        }
    }
    return labels;
}

/// Divide out large-scale variation in the lighting.
///
/// Estimates the illumination as a heavily blurred greyscale copy of the frame
/// and divides the image by it. Hue survives, which is all the classifier reads
/// — so the lit and shadowed sides of one can end up the same colour instead of
/// two different drinks.
cv::Mat flat_field(const cv::Mat &bgr, int blur_radius)
{
    cv::Mat as_float;
    bgr.convertTo(as_float, CV_32F, 1.0 / 255.0);

    cv::Mat grey;
    cv::cvtColor(bgr, grey, cv::COLOR_BGR2GRAY);

    const int k = blur_radius | 1;      // GaussianBlur demands an odd kernel
    cv::Mat illumination;
    cv::GaussianBlur(grey, illumination, cv::Size(k, k), 0);
    illumination.convertTo(illumination, CV_32F, 1.0 / 255.0);
    illumination += 0.05f;              // no divide-by-zero in near-black areas

    std::vector<cv::Mat> channels;
    cv::split(as_float, channels);
    for (cv::Mat &channel : channels) {
        cv::divide(channel, illumination, channel);
    }

    cv::Mat merged;
    cv::merge(channels, merged);

    // Dividing can push bright pixels past 1.0. Rescale rather than clip, so a
    // highlight keeps its colour instead of saturating to white — white is
    // exactly what the classifier is meant to reject as background.
    double max_value = 0.0;
    cv::minMaxLoc(merged, nullptr, &max_value);
    if (max_value > 1.0) merged /= max_value;

    cv::Mat out;
    merged.convertTo(out, CV_8U, 255.0);
    return out;
}

/// Isolate one brand's pixels and tidy the mask.
void build_mask(int brand_index, Frames &frames, const vision::Config &config)
{
    frames.mask = (frames.classification == brand_index);
    frames.morphology = frames.mask.clone();

    if (config.open_kernel > 0) {
        cv::morphologyEx(
            frames.morphology, frames.morphology, cv::MORPH_OPEN,
            cv::getStructuringElement(
                cv::MORPH_ELLIPSE,
                cv::Size(config.open_kernel, config.open_kernel)));
    }

    if (config.close_kernel > 0) {
        // Tall, not square, and that asymmetry is deliberate: a can is a tall
        // thin blob, and a band of glare across the middle splits it in two.
        // Closing vertically rejoins it without bridging two cans standing
        // side by side, which a square kernel of the same reach would.
        cv::morphologyEx(
            frames.morphology, frames.morphology, cv::MORPH_CLOSE,
            cv::getStructuringElement(
                cv::MORPH_ELLIPSE,
                cv::Size(config.close_kernel,
                         config.close_kernel * vision::Config::CLOSE_ASPECT)));
    }
}

/// Count the blobs in the current mask, and optionally draw them.
///
/// COUNTING AND DRAWING ARE ONE PASS BUT NOT ONE DECISION. The previous version
/// took a "what to draw" flag and returned early when it was NONE — so a
/// headless run found contours, drew none of them, and reported every count as
/// zero. Nothing said so; the packets were well-formed and entirely wrong.
/// `annotate` now suppresses the drawing alone.
std::vector<Detection> find_cans(Frames &frames, const vision::Config &config,
                                 const vision::Brand &brand, bool annotate)
{
    std::vector<Detection> found;
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(frames.morphology, contours, cv::RETR_EXTERNAL,
                     cv::CHAIN_APPROX_SIMPLE);

    for (const std::vector<cv::Point> &contour : contours) {
        // Simplify before measuring: a mask edge is jagged at the pixel level
        // and the raw perimeter is dominated by that noise.
        std::vector<cv::Point> simplified;
        const double epsilon = vision::Config::APPROX_EPSILON_FRACTION *
                               cv::arcLength(contour, true);
        cv::approxPolyDP(contour, simplified, epsilon, true);

        const double area = cv::contourArea(simplified);
        if (area < config.contour_min_area) continue;
        if (area > config.contour_max_area) continue;

        const cv::Moments moments = cv::moments(simplified);
        if (moments.m00 <= 0) continue;

        Detection detection;
        detection.area = area;
        detection.centroid = cv::Point2f((float)(moments.m10 / moments.m00),
                                         (float)(moments.m01 / moments.m00));
        detection.bbox = cv::boundingRect(simplified);
        found.push_back(detection);

        if (annotate) {
            cv::rectangle(frames.original, detection.bbox,
                          cv::Scalar(0, 255, 0), 2);
            cv::circle(frames.original, detection.centroid, 5,
                       cv::Scalar(0, 0, 255), -1);
            cv::putText(frames.original, brand.name, detection.centroid,
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 255), 2);
        }
    }
    return found;
}

/// `coke:5,fanta:4,mtndew:5,solo:3;`
///
/// Wire keys rather than initials, so the reader needs no letter-to-drink table
/// of its own to keep in step with catalogue.h.
std::string serialise(const std::vector<std::vector<Detection>> &per_brand,
                      const vision::Config &config)
{
    std::ostringstream out;
    for (size_t i = 0; i < per_brand.size(); i++) {
        if (i != 0) out << ",";
        out << config.brands[i].wire_key << ":" << per_brand[i].size();
    }
    out << ";";
    return out.str();
}

// ---------------------------------------------------------------------------
// The debug UI. None of this runs headless.
// ---------------------------------------------------------------------------

void on_mouse(int event, int x, int y, int /*flags*/, void *userdata)
{
    SampleState *state = static_cast<SampleState *>(userdata);
    if (event != cv::EVENT_LBUTTONDOWN || state->frame == nullptr) return;
    if (state->frame->empty()) return;
    if (x < 0 || y < 0 || x >= state->frame->cols || y >= state->frame->rows) {
        return;
    }

    const cv::Vec3b bgr = state->frame->at<cv::Vec3b>(y, x);   // row=y, col=x
    cv::Mat one_pixel(1, 1, CV_8UC3, cv::Scalar(bgr[0], bgr[1], bgr[2]));
    cv::Mat converted;
    cv::cvtColor(one_pixel, converted, cv::COLOR_BGR2HSV);
    const cv::Vec3b hsv = converted.at<cv::Vec3b>(0, 0);

    if (state->clicks == 0) {
        state->first = hsv;
        state->clicks = 1;
        printf("  sample 1 of 2: H=%d S=%d V=%d\n", hsv[0], hsv[1], hsv[2]);
    } else {
        state->second = hsv;
        state->clicks = 0;
        state->pair_ready = true;
        printf("  sample 2 of 2: H=%d S=%d V=%d\n", hsv[0], hsv[1], hsv[2]);
    }

    // --- Why this pixel classified the way it did ---
    //
    // The readout that makes two similar drinks tunable. "Coke sometimes reads
    // as Fanta" is not actionable; "this pixel is 31.2 from Coke and 33.8 from
    // Fanta" says exactly how much margin there is and which channel spent it.
    // Printing the per-channel contributions matters as much as the total —
    // that is how the original fault was visible at a glance, with the value
    // term larger than the hue term for two drinks that differ only in hue.
    if (state->config != nullptr && state->centres != nullptr) {
        const vision::Config &config = *state->config;

        if (!is_candidate(hsv, config)) {
            printf("      background: S=%d V=%d is below the floor "
                   "(min_saturation=%d min_value=%d)\n",
                   hsv[1], hsv[2], config.min_saturation, config.min_value);
        } else {
            double best = std::numeric_limits<double>::max();
            size_t winner = 0;
            for (size_t i = 0; i < state->centres->size(); i++) {
                const double dist = brand_distance(hsv, (*state->centres)[i],
                                                   config);
                if (dist < best) {
                    best = dist;
                    winner = i;
                }
            }

            for (size_t i = 0; i < state->centres->size(); i++) {
                const BrandCentre &centre = (*state->centres)[i];
                const double dh = hue_distance(hsv[0], centre.h) *
                                  config.hue_weight;
                const double ds = ((double)hsv[1] - centre.s) *
                                  config.sat_weight;
                const double dv = ((double)hsv[2] - centre.v) *
                                  config.val_weight;
                const double dist = brand_distance(hsv, centre, config);

                printf("      %s %-14s dist %6.1f  (hue %5.1f  sat %5.1f  "
                       "val %5.1f)\n",
                       (i == winner && dist <= config.max_brand_dist) ? "->"
                                                                      : "  ",
                       config.brands[i].name.c_str(), dist,
                       dh, std::fabs(ds), std::fabs(dv));
            }
            if (best > config.max_brand_dist) {
                printf("      -> background (nearest is %.1f, over "
                       "max_brand_dist=%.1f)\n", best, config.max_brand_dist);
            }
        }
    }
    fflush(stdout);
}

/// Set the selected brand's band from the two clicked pixels.
///
/// A thin adapter: the arithmetic lives in vision_config so that the hue
/// wrap-around it has to get right can be tested without a camera.
void apply_sample(vision::Brand &brand, const cv::Vec3b &a, const cv::Vec3b &b)
{
    vision::apply_sample(brand, a[0], a[1], a[2], b[0], b[1], b[2]);
}

/// Could these two samples plausibly have come from the same can?
bool sample_pair_is_consistent(const cv::Vec3b &a, const cv::Vec3b &b)
{
    return vision::samples_agree(a[0], b[0]);
}

void create_windows(Mode mode, vision::Config &config, SampleState &sampling)
{
    if (mode == Mode::Headless) return;

    cv::namedWindow(WIN_CAMERA, cv::WINDOW_NORMAL);
    cv::setMouseCallback(WIN_CAMERA, on_mouse, &sampling);

    if (mode != Mode::DebugAll) return;

    cv::namedWindow(WIN_MASK, cv::WINDOW_NORMAL);
    cv::namedWindow(WIN_MORPH, cv::WINDOW_NORMAL);
    cv::namedWindow(WIN_FLAT, cv::WINDOW_NORMAL);
    cv::namedWindow(WIN_CONTROL, cv::WINDOW_AUTOSIZE);

    // Bound straight to the config's own fields, so a slider and the file can
    // never disagree about what is currently in force and saving writes exactly
    // what the sliders show. Even kernel values are snapped to odd each pass —
    // see the call to vision::normalise() in the loop.
    cv::createTrackbar("open", WIN_CONTROL, &config.open_kernel,
                       vision::Config::MAX_KERNEL);
    cv::createTrackbar("close", WIN_CONTROL, &config.close_kernel,
                       vision::Config::MAX_KERNEL);
    cv::createTrackbar("min area", WIN_CONTROL, &config.contour_min_area,
                       vision::Config::MAX_AREA);
    cv::createTrackbar("max area", WIN_CONTROL, &config.contour_max_area,
                       vision::Config::MAX_AREA);

    // The two floors are on sliders because they are the fastest way to see
    // what is being thrown away: wind min_saturation up until the shelf goes
    // black in the classified view, then back off.
    cv::createTrackbar("min sat", WIN_CONTROL, &config.min_saturation, 255);
    cv::createTrackbar("min val", WIN_CONTROL, &config.min_value, 255);
}

/// Say on the picture which drink the next clicks will retune.
///
/// ON THE PICTURE, not in the console, and that distinction is the whole point.
/// The armed brand was announced once, to a console that scrolls a count line
/// several times a second — so by the time anyone clicked, the message naming
/// what they were about to overwrite had long gone. Solo was silently retuned
/// onto a Coke, a Fanta and a Mountain Dew that way.
void draw_tuning_overlay(cv::Mat &frame, const vision::Config &config,
                         const SampleState &sampling)
{
    if (frame.empty() || config.brands.empty()) return;
    if (sampling.brand_index < 0 ||
        (size_t)sampling.brand_index >= config.brands.size()) {
        return;
    }

    const std::string label =
        "TUNING: " + config.brands[sampling.brand_index].name +
        (sampling.clicks == 1 ? "   [click 2 of 2]" : "   [click 1 of 2]");

    // Drawn twice, dark then light, so it stays readable against both a bright
    // can and the dark inside of the fridge.
    cv::putText(frame, label, cv::Point(8, 22), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                cv::Scalar(0, 0, 0), 4);
    cv::putText(frame, label, cv::Point(8, 22), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                cv::Scalar(255, 255, 255), 1);
}

void show_windows(Mode mode, const Frames &frames)
{
    if (mode == Mode::Headless) return;

    cv::imshow(WIN_CAMERA, frames.original);
    if (mode != Mode::DebugAll) return;

    // The mask and morphology views show the LAST brand of the frame, which is
    // the one the loop left behind. Worth knowing when reading them: to inspect
    // a particular drink, put it last in the config, or read the labels on the
    // camera view instead.
    if (!frames.mask.empty()) cv::imshow(WIN_MASK, frames.mask);
    if (!frames.morphology.empty()) cv::imshow(WIN_MORPH, frames.morphology);
    if (!frames.flattened.empty()) cv::imshow(WIN_FLAT, frames.flattened);
}

void print_config(const vision::Config &config, int selected)
{
    printf("  tuning:\n");
    for (size_t i = 0; i < config.brands.size(); i++) {
        const vision::Brand &brand = config.brands[i];
        printf("    %s %zu %-14s H %3d-%3d  S %3d-%3d  V %3d-%3d\n",
               (int)i == selected ? "->" : "  ", i + 1, brand.name.c_str(),
               brand.lowH, brand.highH, brand.lowS, brand.highS,
               brand.lowV, brand.highV);
    }
    printf("    weights: hue=%.2f sat=%.2f val=%.2f   max_dist=%.1f\n",
           config.hue_weight, config.sat_weight, config.val_weight,
           config.max_brand_dist);
    printf("    floors:  min_saturation=%d min_value=%d\n",
           config.min_saturation, config.min_value);
    printf("    mask:    open=%d close=%d area=%d-%d\n",
           config.open_kernel, config.close_kernel, config.contour_min_area,
           config.contour_max_area);
    fflush(stdout);
}

void print_debug_keys(const vision::Config &config)
{
    printf("\n--- tuning keys (the camera window must have focus) ---\n");
    printf("  1-%zu   choose which drink the next two clicks tune\n",
           config.brands.size());
    printf("  click  twice on ONE can - its brightest part, then its dullest\n");
    printf("         (the drink being tuned is shown on the camera window)\n");
    printf("  u      undo the last sample\n");
    printf("  r      reset the selected drink to its built-in colour\n");
    printf("  s      save the current tuning to %s\n", CONFIG_PATH);
    printf("  p      print the current tuning\n");
    printf("  q      quit\n\n");
    printf("  counts print only when they CHANGE, so a steady shelf is quiet\n"
           "  and a wandering one is obvious.\n\n");
    fflush(stdout);
}

/// Complain if two drinks are too alike to be told apart reliably.
///
/// A warning rather than a refusal: two similar drinks may genuinely be the
/// best available, and blocking the save would leave nowhere to go. But it is
/// worth saying out loud, because the symptom downstream is not "these two are
/// confusable" — it is one drink's count quietly appearing on the other.
void warn_about_close_centres(const vision::Config &config,
                              const std::vector<BrandCentre> &centres)
{
    for (size_t i = 0; i < centres.size(); i++) {
        for (size_t j = i + 1; j < centres.size(); j++) {
            const double dh = hue_distance(centres[i].h, centres[j].h) *
                              config.hue_weight;
            const double ds = (centres[i].s - centres[j].s) * config.sat_weight;
            const double dv = (centres[i].v - centres[j].v) * config.val_weight;
            const double dist = std::sqrt(dh * dh + ds * ds + dv * dv);

            if (dist < vision::Config::MIN_CENTRE_SEPARATION) {
                printf("  WARNING: %s and %s are only %.1f apart - expect one "
                       "to be counted as the other\n",
                       config.brands[i].name.c_str(),
                       config.brands[j].name.c_str(), dist);
            }
        }
    }
}

// ---------------------------------------------------------------------------

void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s <mode>\n"
            "  --headless       no windows; what the service runs\n"
            "  --debug-camera   the annotated camera view\n"
            "  --debug-all      every stage, plus the tuning sliders\n"
            "\n"
            "Tuning is read from %s if it exists, and from the compiled-in\n"
            "defaults otherwise. The 's' key writes it back.\n",
            program, CONFIG_PATH);
}

/// The GStreamer pipeline, built from the configured resolutions.
std::string build_pipeline(const vision::Config &config)
{
    std::ostringstream out;
    // Captured high and processed low on purpose: the sensor gives a better
    // image downsampled than it does asked for a small one directly, and every
    // stage after this is per-pixel work.
    out << "libcamerasrc";
    if (!config.libcamerasrc_extra.empty()) {
        out << " " << config.libcamerasrc_extra;
    }
    out << " ! video/x-raw, width=" << config.capture_width
        << ", height=" << config.capture_height
        << " ! videoconvert"
        << " ! videoscale"
        << " ! video/x-raw, width=" << config.process_width
        << ", height=" << config.process_height;
    if (config.rotate_180) {
        out << " ! videoflip method=rotate-180";
    }
    // drop=true with a shallow queue: when the pipeline falls behind we want
    // the NEWEST frame, not a backlog of stale ones. A count is only worth
    // anything if it describes the shelf as it is now.
    out << " ! appsink drop=true max_buffers=2";
    return out.str();
}

} // namespace

int main(int argc, char *argv[])
{
    Mode mode = Mode::Headless;
    bool mode_given = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--headless") == 0) {
            mode = Mode::Headless;
        } else if (strcmp(argv[i], "--debug-camera") == 0) {
            mode = Mode::DebugCamera;
        } else if (strcmp(argv[i], "--debug-all") == 0) {
            mode = Mode::DebugAll;
        } else {
            fprintf(stderr, "Unrecognised argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
        mode_given = true;
    }

    if (!mode_given) {
        print_usage(argv[0]);
        return 1;
    }

    // --- Tuning ---
    vision::Config config = vision::defaults();
    std::string error;
    if (vision::load(CONFIG_PATH, config, &error)) {
        fprintf(stderr, "picapture: tuning loaded from %s\n", CONFIG_PATH);
    } else if (!error.empty()) {
        // The file exists but is wrong. Refused rather than ignored: carrying
        // on with the defaults would run with tuning nobody chose, and would
        // look exactly like a file that had been applied.
        fprintf(stderr, "picapture: %s\n", error.c_str());
        return 1;
    } else {
        fprintf(stderr, "picapture: no %s, using built-in defaults\n",
                CONFIG_PATH);
    }

    std::string note;
    if (vision::normalise(config, &note)) {
        fprintf(stderr, "picapture: %s\n", note.c_str());
    }
    if (!vision::validate(config, &error)) {
        fprintf(stderr, "picapture: %s\n", error.c_str());
        return 1;
    }

    std::vector<BrandCentre> centres = build_centres(config);

    // --- Camera ---
    const std::string pipeline = build_pipeline(config);
    // Always printed, not just on failure: when counts drift for no visible
    // reason the first question is what the camera was actually asked for, and
    // libcamerasrc_extra makes that a per-machine answer.
    fprintf(stderr, "picapture: %s\n", pipeline.c_str());

    cv::VideoCapture camera;
    camera.open(pipeline, cv::CAP_GSTREAMER);
    if (!camera.isOpened()) {
        fprintf(stderr,
                "picapture: could not open the camera.\n"
                "  pipeline: %s\n"
                "  Check that gstreamer1.0-libcamera is installed and that\n"
                "  this user is in the 'video' group.\n",
                pipeline.c_str());
        return 1;
    }

    Frames frames;
    SampleState sampling;
    sampling.frame = &frames.original;
    sampling.config = &config;
    sampling.centres = &centres;

    create_windows(mode, config, sampling);
    if (mode != Mode::Headless) {
        print_debug_keys(config);
        print_config(config, sampling.brand_index);
    }

    // The last band a sample overwrote, so a misclick costs one keystroke
    // rather than a retune. Only one level deep on purpose: the recovery that
    // matters is undoing the click you just watched go wrong.
    vision::Brand undo_brand;
    int undo_index = -1;

    /// Last packet printed, for the debug modes' print-on-change.
    std::string last_packet;

    using clock = std::chrono::steady_clock;
    auto next_due = clock::now();
    const auto period = std::chrono::milliseconds(config.period_ms);

    bool running = true;
    while (running) {
        next_due += period;

        if (!camera.read(frames.original) || frames.original.empty()) {
            fprintf(stderr, "picapture: the camera stopped returning frames\n");
            return 1;
        }

        if (config.flat_field_blur > 0) {
            frames.flattened = flat_field(frames.original,
                                          config.flat_field_blur);
            cv::cvtColor(frames.flattened, frames.hsv, cv::COLOR_BGR2HSV);
        } else {
            cv::cvtColor(frames.original, frames.hsv, cv::COLOR_BGR2HSV);
        }

        frames.classification = classify(frames.hsv, centres, config);

        // Drawing is the largest per-frame cost after classification, and
        // headless has nobody to show it to.
        const bool annotate = (mode != Mode::Headless);

        std::vector<std::vector<Detection>> per_brand;
        per_brand.reserve(config.brands.size());
        for (size_t i = 0; i < config.brands.size(); i++) {
            build_mask((int)i, frames, config);
            per_brand.push_back(
                find_cans(frames, config, config.brands[i], annotate));
        }

        // The one line anybody downstream reads. std::endl rather than '\n'
        // because stdout is a PIPE here, not a terminal: a block-buffered pipe
        // would hold counts back until the buffer filled, which at one short
        // line per period is minutes.
        //
        // Headless emits every period, because that is the protocol and a
        // reader needs to know the counts are still current. The debug modes
        // print only on CHANGE: a count line several times a second scrolls
        // every diagnostic off the screen faster than it can be read, which is
        // exactly how a tuning session goes wrong without anybody noticing.
        // Printing on change also makes instability visible rather than
        // something to spot in a wall of identical lines.
        const std::string packet = serialise(per_brand, config);
        if (mode == Mode::Headless) {
            std::cout << packet << std::endl;
        } else if (packet != last_packet) {
            std::cout << packet << std::endl;
            last_packet = packet;
        }

        if (mode != Mode::Headless) {
            draw_tuning_overlay(frames.original, config, sampling);
        }
        show_windows(mode, frames);

        if (mode != Mode::Headless) {
            // waitKey is what pumps the window event loop, so it has to be
            // called every frame whether or not a key is wanted — and must NOT
            // be called headless, where there is no HighGUI display to pump.
            const int key = cv::waitKey(1);

            if (key == 'q') {
                running = false;
            } else if (key == 's') {
                vision::normalise(config, nullptr);
                if (vision::validate(config, &error) &&
                    vision::save(CONFIG_PATH, config, &error)) {
                    printf("  saved to %s\n", CONFIG_PATH);
                    warn_about_close_centres(config, centres);
                } else {
                    printf("  NOT saved: %s\n", error.c_str());
                }
                fflush(stdout);
            } else if (key == 'p') {
                print_config(config, sampling.brand_index);
            } else if (key == 'u') {
                if (undo_index >= 0) {
                    config.brands[undo_index] = undo_brand;
                    centres = build_centres(config);
                    printf("  undone: %s is back to its previous colour\n",
                           undo_brand.name.c_str());
                    undo_index = -1;
                    print_config(config, sampling.brand_index);
                } else {
                    printf("  nothing to undo\n");
                    fflush(stdout);
                }
            } else if (key == 'r') {
                // Matched by wire key rather than by position: a config that
                // reordered its brands would otherwise reset the wrong drink,
                // which is the same class of mistake this whole change is about.
                const vision::Config built_in = vision::defaults();
                const std::string &key_wanted =
                    config.brands[sampling.brand_index].wire_key;
                bool found = false;
                for (const vision::Brand &brand : built_in.brands) {
                    if (brand.wire_key == key_wanted) {
                        undo_brand = config.brands[sampling.brand_index];
                        undo_index = sampling.brand_index;
                        config.brands[sampling.brand_index] = brand;
                        centres = build_centres(config);
                        found = true;
                        break;
                    }
                }
                printf(found ? "  reset to the built-in colour\n"
                             : "  no built-in colour for this drink\n");
                if (found) print_config(config, sampling.brand_index);
                fflush(stdout);
            } else if (key >= '1' && key <= '9') {
                const size_t chosen = (size_t)(key - '1');
                if (chosen < config.brands.size()) {
                    sampling.brand_index = (int)chosen;
                    sampling.clicks = 0;
                    printf("  now tuning %s\n",
                           config.brands[chosen].name.c_str());
                    fflush(stdout);
                }
            }

            // A completed pair of clicks retunes the selected brand, and the
            // centres are rebuilt so the NEXT frame already shows the effect.
            // Seeing the change immediately is the whole reason to tune from a
            // window rather than by editing the file.
            if (sampling.pair_ready) {
                sampling.pair_ready = false;

                // Two clicks are meant to be the lit and shadowed sides of ONE
                // can. Hues this far apart are two different drinks, and
                // applying them would drag this brand's centre to a colour
                // halfway between two others — where it starts claiming both.
                // Refused rather than warned about: the pair carries no usable
                // information either way, and a warning in a scrolling console
                // is what failed last time.
                if (!sample_pair_is_consistent(sampling.first,
                                               sampling.second)) {
                    printf("  IGNORED: those two clicks are %.0f apart in hue, "
                           "so they are not the same can.\n"
                           "  %s was left alone. Check the drink named on the "
                           "camera window, then click twice on ONE can.\n",
                           hue_distance(sampling.first[0], sampling.second[0]),
                           config.brands[sampling.brand_index].name.c_str());
                    fflush(stdout);
                } else {
                    undo_brand = config.brands[sampling.brand_index];
                    undo_index = sampling.brand_index;
                    apply_sample(config.brands[sampling.brand_index],
                                 sampling.first, sampling.second);
                    centres = build_centres(config);
                    print_config(config, sampling.brand_index);
                }
            }

            // Sliders write straight into the config, so an even kernel can
            // appear between frames. Snapped here rather than at the point of
            // use, so what the next frame does and what a save would write are
            // the same number.
            vision::normalise(config, nullptr);
        }

        // Fell behind: drop the beat rather than bursting to catch up.
        // Bursting would spend the recovery running the most expensive stage
        // back to back on a Pi that is evidently already struggling.
        if (clock::now() > next_due) next_due = clock::now();
        std::this_thread::sleep_until(next_due);
    }

    camera.release();
    if (mode != Mode::Headless) cv::destroyAllWindows();
    return 0;
}
