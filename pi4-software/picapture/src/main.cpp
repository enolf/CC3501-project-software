// picapture: what is on the shelf, worked out from a camera.
//
// Reads the Pi camera, decides how many of each drink it can see, and prints
// one line per look:
//
//     coke:5,fanta:4,mtndew:5,solo:3;conf=87;
//
// The `conf=` is how much to believe that line, 0-100. It exists because a
// miscounted shelf is otherwise INVISIBLE: every other fault in this system
// announces itself — a corrupt serial frame fails its CRC, a payment that never
// happened has no row — but a wrong count produces a perfectly well-formed
// packet with the wrong numbers in it. The only evidence available is how
// equivocal the measurement was, so it is measured and carried. See
// vision::Quality.
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
//   6. score                how equivocal was any of that
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

/// Click-to-sample: two clicks on a can set that drink's colour.
///
/// Each click contributes a PATCH of pixels, not one. See vision::SampleSet
/// for why — two lone pixels were not enough to describe a can, and the
/// failures were silent.
struct SampleState {
    const cv::Mat *frame = nullptr;  ///< The image clicks are read from
    int brand_index = 0;             ///< Which brand the clicks tune
    int clicks = 0;
    vision::SampleSet samples;
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
        centres.push_back({(double)brand.hue, (double)brand.saturation,
                           (double)brand.value});
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

/// What the frame looked like, for telling a dark picture from an empty shelf.
struct FrameStats {
    long candidates = 0;    ///< Pixels that passed the saturation/value floors
    long pixels = 0;
    double mean_value = 0.0;
};

/// Assign every pixel to its nearest brand, or to the background.
///
/// One pass covering all brands rather than one pass per brand: cheaper, and —
/// the reason it was written this way — it is what guarantees a pixel has a
/// single owner.
cv::Mat classify(const cv::Mat &hsv, const std::vector<BrandCentre> &centres,
                 const vision::Config &config, FrameStats &stats)
{
    cv::Mat labels(hsv.size(), CV_8SC1, cv::Scalar(BACKGROUND_LABEL));

    stats = FrameStats{};
    stats.pixels = (long)hsv.rows * hsv.cols;
    long value_total = 0;

    for (int y = 0; y < hsv.rows; ++y) {
        const cv::Vec3b *row = hsv.ptr<cv::Vec3b>(y);
        signed char *out = labels.ptr<signed char>(y);

        for (int x = 0; x < hsv.cols; ++x) {
            const cv::Vec3b &px = row[x];
            value_total += px[2];

            if (!is_candidate(px, config)) continue;   // shelf, shadow, glare
            stats.candidates++;

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

    if (stats.pixels > 0) {
        stats.mean_value = (double)value_total / (double)stats.pixels;
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
                                 const vision::Brand &brand, bool annotate,
                                 vision::Quality &quality)
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

        // A rejected blob is not nothing. It is a region of this drink's colour
        // that the pipeline saw and then decided not to account for, and it is
        // counted so the frame's confidence can say so — a discard that leaves
        // no trace anywhere is how a count goes wrong quietly.
        //
        // Only the ones big enough to have been a can, though. Every mask
        // contains dozens of few-pixel fragments and none of them says anything
        // about the count; counting those would peg the deduction at its cap
        // every single frame and the figure would stop varying at all.
        const double area = cv::contourArea(simplified);
        if (area < config.contour_min_area) {
            if (area >= config.contour_min_area *
                            vision::Config::CONF_NOTABLE_REJECT_FRACTION) {
                quality.rejected_small++;
            }
            continue;
        }
        if (area > config.contour_max_area) {
            quality.rejected_large++;
            continue;
        }

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

/// How many cans one brand's blobs represent.
///
/// NOT simply the number of blobs. Two cans of the same drink standing against
/// each other are one connected region of colour, and counting blobs reports
/// them as one can — silently, with a perfectly well-formed packet. Because the
/// firmware charges for the difference between two scans, a merge that appears
/// or disappears across a door cycle invents or hides a purchase.
///
/// Area is very nearly linear in the number of cans, so dividing by the area of
/// one can recovers the count. Rounded rather than truncated, and floored at
/// one, so a partly occluded can still counts as the one can it is.
///
/// Falls back to one-per-blob when the brand has not been calibrated, which is
/// the previous behaviour and the right default: an uncalibrated divisor would
/// be a guess applied to every count.
int count_cans(const std::vector<Detection> &blobs, const vision::Brand &brand)
{
    if (brand.can_area <= 0) {
        return (int)blobs.size();
    }

    int total = 0;
    for (const Detection &blob : blobs) {
        const int cans = (int)std::lround(blob.area / (double)brand.can_area);
        total += std::max(1, cans);
    }
    return total;
}

/// Record how cleanly one brand's blobs divide into whole cans.
///
/// The most direct evidence available about whether a count is right. Area is
/// nearly linear in the number of cans, so a blob at 1.02× or 1.98× the size of
/// one can is telling you plainly what it is — and a blob at 1.5× is a coin
/// toss whose outcome decides whether somebody gets charged. Nothing else in
/// the pipeline can see that distinction: both round to a number and both
/// produce an identical, entirely well-formed packet.
///
/// An uncalibrated brand contributes nothing here, because there is nothing to
/// divide by. That is not free — `confidence()` deducts for it separately,
/// rather than letting a brand that cannot be checked look as sure as one that
/// was checked and passed.
void tally_areas(const std::vector<Detection> &blobs,
                 const vision::Brand &brand, vision::Quality &quality)
{
    quality.brands++;

    if (brand.can_area <= 0) {
        quality.uncalibrated++;
        return;
    }

    for (const Detection &blob : blobs) {
        const double ratio = blob.area / (double)brand.can_area;

        // Floored at one can, matching count_cans: a blob at 0.3× is still
        // counted as the one can it presumably is. The gap is capped at 0.5
        // because that is already maximally ambiguous — being further out than
        // halfway does not make it more of a coin toss, it makes it a blob that
        // is the wrong size, which the area filter is the thing to complain
        // about.
        const double whole = std::max(1.0, std::round(ratio));
        quality.ambiguity_sum += std::min(0.5, std::fabs(ratio - whole));
        quality.ambiguity_blobs++;
    }
}

/// `coke:5,fanta:4,mtndew:5,solo:3;`
///
/// Wire keys rather than initials, so the reader needs no letter-to-drink table
/// of its own to keep in step with catalogue.h.
///
/// The counts ALONE. `conf=` is appended by the caller rather than added here,
/// because the debug modes print only when the counts change and the confidence
/// figure moves a point or two every frame — folded into one string, "print on
/// change" would print on every frame and the quiet-shelf behaviour that makes
/// instability visible would be gone.
std::string serialise(const std::vector<std::vector<Detection>> &per_brand,
                      const vision::Config &config)
{
    std::ostringstream out;
    for (size_t i = 0; i < per_brand.size(); i++) {
        if (i != 0) out << ",";
        out << config.brands[i].wire_key << ":"
            << count_cans(per_brand[i], config.brands[i]);
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

    // A PATCH, not the pixel under the cursor. One pixel is a sample of size
    // one: a glare speck or the dark line between two cans carries the same
    // weight as a representative pixel, and the answer moves depending on
    // exactly where the mouse landed.
    const int radius = vision::Config::SAMPLE_PATCH_RADIUS;
    const int x0 = std::max(0, x - radius);
    const int y0 = std::max(0, y - radius);
    const int x1 = std::min(state->frame->cols - 1, x + radius);
    const int y1 = std::min(state->frame->rows - 1, y + radius);

    cv::Mat patch = (*state->frame)(cv::Range(y0, y1 + 1),
                                    cv::Range(x0, x1 + 1));
    cv::Mat patch_hsv;
    cv::cvtColor(patch, patch_hsv, cv::COLOR_BGR2HSV);

    for (int py = 0; py < patch_hsv.rows; py++) {
        const cv::Vec3b *row = patch_hsv.ptr<cv::Vec3b>(py);
        for (int px = 0; px < patch_hsv.cols; px++) {
            state->samples.add(row[px][0], row[px][1], row[px][2]);
        }
    }

    // Reported from the whole set so far, so the second click shows what the
    // pair actually amounts to rather than just what was under the cursor.
    const cv::Vec3b hsv = patch_hsv.at<cv::Vec3b>(patch_hsv.rows / 2,
                                                  patch_hsv.cols / 2);
    state->clicks++;
    printf("  sample %d of 2: centre pixel H=%d S=%d V=%d, %zu pixels so far, "
           "hue spread %.0f\n",
           state->clicks, hsv[0], hsv[1], hsv[2], state->samples.size(),
           state->samples.hue_spread());

    if (state->clicks >= 2) {
        state->clicks = 0;
        state->pair_ready = true;
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
        printf("    %s %zu %-14s H %3d  S %3d  V %3d   can=%s\n",
               (int)i == selected ? "->" : "  ", i + 1, brand.name.c_str(),
               brand.hue, brand.saturation, brand.value,
               brand.can_area > 0 ? std::to_string(brand.can_area).c_str()
                                  : "not set");
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
    printf("  a      print the size of every blob being counted right now,\n");
    printf("         and what the frame's confidence figure is made of\n");
    printf("  c      with ONE can of the selected drink in view: record how\n");
    printf("         big a single can is, so touching cans count separately\n");
    printf("  u      undo the last sample\n");
    printf("  r      reset the selected drink to its built-in colour\n");
    printf("  s      save the current tuning to %s\n", CONFIG_PATH);
    printf("  p      print the current tuning\n");
    printf("  q      quit\n\n");
    printf("  counts print only when they CHANGE, so a steady shelf is quiet\n"
           "  and a wandering one is obvious.\n\n");
    fflush(stdout);
}

/// Print the size of every blob currently being counted.
///
/// The instrument for the one question this approach cannot answer on its own:
/// a count is a number of BLOBS, not a number of cans, so two cans of the same
/// drink touching each other merge into a single blob and report as one. From
/// the count alone that is indistinguishable from a can having been taken.
///
/// The areas tell them apart immediately. Two separate cans read as two blobs
/// of roughly equal area; the same two touching read as one blob of about
/// twice that. It is also how contour_min_area and contour_max_area get set
/// from measurements rather than from guesses.
void print_areas(const std::vector<std::vector<Detection>> &per_brand,
                 const vision::Config &config, const FrameStats &stats,
                 const vision::Quality &quality)
{
    // Exposure first, because if this is wrong nothing below means anything.
    printf("  picture: mean brightness %.0f of 255, %ld of %ld pixels "
           "(%.1f%%) could be a drink\n",
           stats.mean_value, stats.candidates, stats.pixels,
           stats.pixels > 0 ? 100.0 * stats.candidates / stats.pixels : 0.0);
    printf("  blob areas (min=%d max=%d):\n", config.contour_min_area,
           config.contour_max_area);
    for (size_t i = 0; i < per_brand.size(); i++) {
        const vision::Brand &brand = config.brands[i];
        printf("    %-14s %zu blob(s) -> %d can(s)", brand.name.c_str(),
               per_brand[i].size(), count_cans(per_brand[i], brand));
        if (brand.can_area > 0) {
            printf(" [one can = %d]", brand.can_area);
        } else {
            printf(" [uncalibrated: press c]");
        }
        printf(":");
        if (per_brand[i].empty()) {
            printf("  -");
        }
        for (const Detection &detection : per_brand[i]) {
            printf("  %.0f", detection.area);
        }
        printf("\n");
    }

    // The same arithmetic that produced the `conf=` on the wire, written out.
    // A confidence figure nobody can account for is one nobody will act on: the
    // useful question is never "is 62 bad" but "which of these four things cost
    // the 38", and that is answerable only if the deductions are shown.
    std::string why;
    const int score = vision::confidence(quality, &why);
    printf("  confidence %d: %s\n", score, why.c_str());
    if (quality.rejected_small > 0 || quality.rejected_large > 0) {
        printf("    (%d discarded for being under min area, %d for being over "
               "max - only ones at least %.0f%% of min area are counted)\n",
               quality.rejected_small, quality.rejected_large,
               100.0 * vision::Config::CONF_NOTABLE_REJECT_FRACTION);
    }
    fflush(stdout);
}

/// Record the size of one can of the selected drink, from what is on screen.
///
/// Takes the LARGEST blob rather than an average of them. The instruction is to
/// show exactly one can, so there should be one blob; if a reflection or a
/// sliver of something else also got through, the can is the big one. An
/// average would quietly be dragged down by the very noise the area filter
/// exists to reject.
bool calibrate_can_area(const std::vector<Detection> &blobs,
                        vision::Brand &brand)
{
    double largest = 0.0;
    for (const Detection &blob : blobs) {
        largest = std::max(largest, blob.area);
    }
    if (largest <= 0.0) {
        return false;
    }
    brand.can_area = (int)std::lround(largest);
    return true;
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

            const double too_close = config.max_brand_dist *
                                     vision::Config::MIN_CENTRE_SEPARATION_FRACTION;
            if (dist < too_close) {
                const double hue_gap = hue_distance(centres[i].h, centres[j].h);
                printf("  NOTE: %s and %s are %.1f apart (only %.0f hue).\n"
                       "        Their colours really are that similar, so this "
                       "may be as good as it gets.\n"
                       "        What matters is the margin on a real pixel: "
                       "click a can and check the\n"
                       "        winner is comfortably ahead. If it is not, "
                       "raise hue_weight (now %.1f)\n"
                       "        so hue counts for more than brightness.\n",
                       config.brands[i].name.c_str(),
                       config.brands[j].name.c_str(), dist, hue_gap,
                       config.hue_weight);
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

    /// Last counts printed, for the debug modes' print-on-change. The COUNTS,
    /// not the whole packet: confidence moves a point or two every frame, so
    /// including it would make every frame a change.
    std::string last_counts;

    /// Consecutive frames with almost nothing bright enough to be a drink.
    int dark_frames = 0;

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

        FrameStats stats;
        frames.classification = classify(frames.hsv, centres, config, stats);

        // A nearly-empty frame and a nearly-black one produce identical output:
        // all zeros. Saying which it is turns an afternoon of retuning
        // something that was never wrong into one line on stderr.
        if (stats.candidates <
            (long)(stats.pixels * vision::Config::DARK_FRAME_CANDIDATE_FRACTION)) {
            dark_frames++;
            if (dark_frames == vision::Config::DARK_FRAME_WARN_AFTER ||
                (dark_frames > vision::Config::DARK_FRAME_WARN_AFTER &&
                 dark_frames % vision::Config::DARK_FRAME_WARN_REPEAT == 0)) {
                fprintf(stderr,
                        "picapture: almost nothing in the picture is bright or "
                        "colourful enough to be a drink\n"
                        "  (%ld of %ld pixels pass the floors; mean brightness "
                        "%.0f of 255, min_value=%d min_saturation=%d)\n"
                        "  An empty shelf looks like this. So does an "
                        "underexposed camera - if the mean brightness is low,\n"
                        "  suspect the exposure, NOT the colour tuning. Setting "
                        "ae-enable=false without also giving\n"
                        "  exposure-time and analogue-gain leaves the sensor in "
                        "manual mode with no values to use.\n",
                        stats.candidates, stats.pixels, stats.mean_value,
                        config.min_value, config.min_saturation);
            }
        } else {
            dark_frames = 0;
        }

        // Drawing is the largest per-frame cost after classification, and
        // headless has nobody to show it to.
        const bool annotate = (mode != Mode::Headless);

        vision::Quality quality;
        quality.mean_value = stats.mean_value;

        std::vector<std::vector<Detection>> per_brand;
        per_brand.reserve(config.brands.size());
        for (size_t i = 0; i < config.brands.size(); i++) {
            build_mask((int)i, frames, config);
            per_brand.push_back(
                find_cans(frames, config, config.brands[i], annotate, quality));
            tally_areas(per_brand.back(), config.brands[i], quality);
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
        //
        // The change is judged on the COUNTS, not on the whole line. Confidence
        // drifts by a point or two every frame, so comparing the full packet
        // would make every frame a change and the quiet shelf would go away.
        const std::string counts = serialise(per_brand, config);
        const std::string packet =
            counts + "conf=" + std::to_string(vision::confidence(quality)) +
            ";";

        if (mode == Mode::Headless) {
            std::cout << packet << std::endl;
        } else if (counts != last_counts) {
            std::cout << packet << std::endl;
            last_counts = counts;
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
            } else if (key == 'a') {
                print_areas(per_brand, config, stats, quality);
            } else if (key == 'c') {
                vision::Brand &brand = config.brands[sampling.brand_index];
                undo_brand = brand;
                undo_index = sampling.brand_index;
                if (calibrate_can_area(per_brand[sampling.brand_index],
                                       brand)) {
                    printf("  one %s is %d pixels - two touching cans will now "
                           "count as two\n",
                           brand.name.c_str(), brand.can_area);
                } else {
                    printf("  no %s visible to measure\n", brand.name.c_str());
                }
                fflush(stdout);
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

                // to_brand() refuses a set whose hues are spread too wide to
                // be one can — which is what clicking on two different drinks
                // looks like from in there. Refused rather than warned about:
                // the samples carry no usable information either way, and a
                // warning in a scrolling console is what failed before.
                vision::Brand candidate = config.brands[sampling.brand_index];
                std::string why;
                if (!sampling.samples.to_brand(candidate, &why)) {
                    printf("  IGNORED: %s\n  %s was left alone.\n", why.c_str(),
                           config.brands[sampling.brand_index].name.c_str());
                    fflush(stdout);
                } else {
                    undo_brand = config.brands[sampling.brand_index];
                    undo_index = sampling.brand_index;
                    config.brands[sampling.brand_index] = candidate;
                    centres = build_centres(config);
                    print_config(config, sampling.brand_index);
                }
                sampling.samples.clear();
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
