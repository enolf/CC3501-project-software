/* 
Requires OpenCV 4.x - OpenCV 5.0.0 (Homebrew) doesn't work as of July 2026 missing moments class
OpenCV 4.x Homebrew or windows equivelant.
*/

/*TODO
update README
Can distance thresholding
*/

#include <opencv2/opencv.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <ostream>
#include <sys/time.h>
#include <string>
#include <sstream>
#include <chrono>
#include <thread>
#include <limits>
#include <cmath>

#include "trackbar.h"

#define USE_FLAT_IMAGE
#define TESTING
#define PI_CAMERA_MODULE

constexpr int MODE_HEADLESS = 0;
constexpr int MODE_DEBUG_CAMERA = 1;
constexpr int MODE_DEBUG_ALL = 2;

constexpr const char* pipeline = "libcamerasrc"
  " ! video/x-raw, width=800, height=600" // camera needs to capture at a higher resolution
  " ! videoconvert"
  " ! videoscale"
  " ! video/x-raw, width=400, height=300" // can downsample the image after capturing
  " ! videoflip method=rotate-180" // remove this line if the image is upside-down
  " ! appsink drop=true max_buffers=2";

enum visualise_contours_mode {
  NONE = 0,
  BBOX = 1 << 0,
  CENTROIDS = 1 << 1,
  CONTOURS = 1 << 2,
  TEXT = 1 << 3,
  ALL = BBOX | CENTROIDS | CONTOURS | TEXT
};

struct Contour_Info {
  cv::Rect bbox;
  cv::Point2f centroid;
  double area;
};

struct Color_Config{
  std::string name;
  char wire_code;
  int lowH, highH, lowS, highS, lowV, highV;
};

struct HSV_State{
  cv::Vec3b hsv_sample_1;
  cv::Vec3b hsv_sample_2;
  int hsv_state = 0;
};

struct onMouse_UserData{
  bool just_clicked = false;
  cv::Mat* p_frame;
  HSV_State hsv_state;
};

struct Trackbar_State{
  bool has_changed = false;

  int* iLowH;
  int* iHighH;
  int* iLowS;
  int* iHighS;
  int* iLowV;
  int* iHighV;

  int* iOpen;
  int* iClose;
  int* ContourMinSize;
  int* ContourMaxSize;
};

struct Pipeline_Frames{
  cv::Mat bgr_img;
  cv::Mat original;
  cv::Mat hsv_frame;
  cv::Mat thresholded;
  cv::Mat morphology;
  cv::Mat classification; // NEW: per-pixel nearest-brand label map, one frame, shared by every color in the loop
#ifdef USE_FLAT_IMAGE
  cv::Mat flattened;
#endif
};

constexpr const char* PATH_TO_IMAGE = "cans_test/cans_multi_test_1.jpg";
constexpr const char* BACKUP_PATH_TO_IMAGE = "../cans_test/cans_multi_test_1.jpg";

constexpr int BACKGROUND_LABEL = -1;

#ifndef PI_CAMERA_MODULE
int load_test_image(cv::Mat &bgr_out) {
  // Load the image
  bgr_out = cv::imread(PATH_TO_IMAGE);
  if (!bgr_out.data) {
    bgr_out = cv::imread(BACKUP_PATH_TO_IMAGE);
    if (!bgr_out.data) {
      std::cerr << "Failed to load image\n";
      return 1;
    }
  }
  return 0;
};
#endif

#ifdef USE_FLAT_IMAGE
cv::Mat removeHighlightsShadows(const cv::Mat& bgr_input, int blurRadius = 51) {
  // Flat-field correction: estimate local illumination as a heavily blurred
  // grayscale version of the image, then divide the image by it. This
  // normalises away large-scale lighting variation (highlights/shadows)
  // while preserving hue, which is what the downstream HSV classification
  // actually needs.
  cv::Mat bgrF;
  bgr_input.convertTo(bgrF, CV_32F, 1.0 / 255.0);

  cv::Mat gray;
  cv::cvtColor(bgr_input, gray, cv::COLOR_BGR2GRAY);

  int k = blurRadius | 1; // GaussianBlur requires an odd kernel size
  cv::Mat illumination;
  cv::GaussianBlur(gray, illumination, cv::Size(k, k), 0);
  illumination.convertTo(illumination, CV_32F, 1.0 / 255.0);
  illumination += 0.05f; // avoid divide-by-zero in near-black regions

  std::vector<cv::Mat> channels;
  cv::split(bgrF, channels);
  for (auto& ch : channels) {
    cv::divide(ch, illumination, ch);
  }
  cv::Mat flattened;
  cv::merge(channels, flattened);

  // Division can push bright pixels above 1.0 — rescale back into range.
  double maxVal;
  cv::minMaxLoc(flattened, nullptr, &maxVal);
  if (maxVal > 1.0) flattened /= maxVal;

  cv::Mat output;
  flattened.convertTo(output, CV_8U, 255.0);
  return output;
}

#endif
void show_all(const Pipeline_Frames &pf, int mode) {
  switch (mode) {
  case MODE_HEADLESS:
    break;
  case MODE_DEBUG_CAMERA:
    cv::imshow("Camera", pf.original);
    break;
  case MODE_DEBUG_ALL:
    cv::imshow("Camera", pf.original);
    cv::imshow("Camera - Thresholded", pf.thresholded);
    cv::imshow("Camera - Morphology", pf.morphology);
#ifdef USE_FLAT_IMAGE
    cv::imshow("Camera - Flattend", pf.flattened);
#endif
    break;
    default:
    break;
  }
}
// void show_all(const std::vector<cv::Mat>& fs){
//         for (const auto& f : fs){ cv::imshow("", f);}
// }
std::string serialize_image(const std::vector<std::vector<Contour_Info>>& image_info, const std::vector<Color_Config>& color_config){
  std::ostringstream oss;
  for (size_t i = 0; i < image_info.size(); i++){
    if (i != 0){ oss << ","; }
    oss << color_config[i].wire_code << ":" << image_info[i].size();
  }
  oss << ";";
  return oss.str();
};

//Claude wrote the base of this function:
void onMouse(int event, int x, int y, int flags, void* userdata) {
  onMouse_UserData* data = reinterpret_cast<onMouse_UserData*>(userdata);
  if (event != cv::EVENT_LBUTTONDOWN) {return;}

  cv::Vec3b bgr = data->p_frame->at<cv::Vec3b>(y, x); // note: row=y, col=x
  cv::Mat hsvPixel;
  cv::cvtColor(cv::Mat(1, 1, CV_8UC3, bgr), hsvPixel, cv::COLOR_BGR2HSV);
  cv::Vec3b hsv = hsvPixel.at<cv::Vec3b>(0, 0);

  if (data->hsv_state.hsv_state == 0){
    data->hsv_state.hsv_sample_1 = hsv;
    data->hsv_state.hsv_state = 1;
    printf("HSV: (%d, %d, %d)\n", hsv[0], hsv[1], hsv[2]);
  }else{
    data->hsv_state.hsv_sample_2 = hsv;
    data->hsv_state.hsv_state = 0;
    printf("HSV: (%d, %d, %d)\n", hsv[0], hsv[1], hsv[2]);
    data->just_clicked = true;
  }
};

void set_hsv_trackbars(const onMouse_UserData& data, Trackbar_State& tb){
  const cv::Vec3b& sample_1 = data.hsv_state.hsv_sample_1;
  const cv::Vec3b& sample_2 = data.hsv_state.hsv_sample_2;

  // Hue barely moves between the lit and shadowed side of one can, so the
  // two samples under-report its true spread. Pad it harder than S and V.
  const int hue_tolerance = 6;
  const int satuation_tolerance = 15;
  const int value_tolerance = 15;

  int loH = std::min(sample_1[0], sample_2[0]), hiH = std::max(sample_1[0], sample_2[0]);
  int loS = std::min(sample_1[1], sample_2[1]), hiS = std::max(sample_1[1], sample_2[1]);
  int loV = std::min(sample_1[2], sample_2[2]), hiV = std::max(sample_1[2], sample_2[2]);

  *tb.iLowH = std::max(0, loH - hue_tolerance);
  *tb.iHighH = std::min(179, hiH + hue_tolerance);
  *tb.iLowS = std::max(0, loS - satuation_tolerance);
  *tb.iHighS = std::min(255, hiS + satuation_tolerance);
  *tb.iLowV = std::max(0, loV - value_tolerance);
  *tb.iHighV = std::min(255, hiV + value_tolerance);

  cv::setTrackbarPos("LowH", "Control", *tb.iLowH);
  cv::setTrackbarPos("HighH", "Control", *tb.iHighH);
  cv::setTrackbarPos("LowS",  "Control", *tb.iLowS);
  cv::setTrackbarPos("HighS", "Control", *tb.iHighS);
  cv::setTrackbarPos("LowV",  "Control", *tb.iLowV);
  cv::setTrackbarPos("HighV", "Control", *tb.iHighV);
}

// ---------------------------------------------------------------------------
// NEW: nearest-brand pixel classification.
//
// The previous approach ran an independent inRange() per brand against the
// full frame. Because Coke/Sunkist/Fanta/Solo occupy adjacent hue bands,
// glare, JPEG blocking and anti-aliased edges routinely satisfy more than one
// brand's range at once — that's why a single can in the screenshots was
// picking up four different name labels simultaneously.
//
// This function replaces that with a single pass that assigns each pixel to
// whichever brand's HSV centre it is closest to (or to background if it's
// too far from all of them, or too desaturated/dark to be a genuine can
// colour for any brand). Every pixel therefore belongs to at most one brand,
// so the per-color loop below can no longer double-claim the same region.
// ---------------------------------------------------------------------------

struct Brand_Centre {
  double h, s, v;
  int minS, minV; // reject washed-out / shadowed pixels outright, per brand
};

inline double circular_hue_dist(double h1, double h2) {
  double d = std::abs(h1 - h2);
  return std::min(d, 180.0 - d); // OpenCV hue wraps at 180, not 360
}

std::vector<Brand_Centre> build_brand_centres(const std::vector<Color_Config>& colors) {
  std::vector<Brand_Centre> centres;
  centres.reserve(colors.size());
  for (const auto& c : colors) {
    centres.push_back({
      (c.lowH + c.highH) / 2.0,
      (c.lowS + c.highS) / 2.0,
      (c.lowV + c.highV) / 2.0,
      c.lowS,
      c.lowV
    });
  }
  return centres;
}

// hue_weight: how much more a hue mismatch counts than an S/V mismatch.
//   Hue is the reliable discriminator between brands (S/V shift with glare
//   and shadow on the same can), so it should dominate the distance.
// max_dist: pixels further than this from every brand centre are background.
cv::Mat classify_by_nearest_brand(const cv::Mat& hsv_frame,
                                   const std::vector<Brand_Centre>& centres,
                                   double hue_weight = 4.0,
                                   double max_dist = 70.0) {
  cv::Mat labels(hsv_frame.size(), CV_8SC1, cv::Scalar(BACKGROUND_LABEL));

  for (int y = 0; y < hsv_frame.rows; ++y) {
    const cv::Vec3b* row = hsv_frame.ptr<cv::Vec3b>(y);
    schar* out = labels.ptr<schar>(y);
    for (int x = 0; x < hsv_frame.cols; ++x) {
      const cv::Vec3b& px = row[x];
      double bestDist = std::numeric_limits<double>::max();
      int bestLabel = BACKGROUND_LABEL;

      for (size_t i = 0; i < centres.size(); ++i) {
        const Brand_Centre& b = centres[i];
        if (px[1] < b.minS || px[2] < b.minV) continue; // too grey/dark for this brand

        double dh = circular_hue_dist(px[0], b.h) * hue_weight;
        double ds = px[1] - b.s;
        double dv = px[2] - b.v;
        double dist = std::sqrt(dh * dh + ds * ds + dv * dv);

        if (dist < bestDist) {
          bestDist = dist;
          bestLabel = static_cast<int>(i);
        }
      }

      if (bestDist <= max_dist) out[x] = static_cast<schar>(bestLabel);
    }
  }
  return labels;
}

// std::vector<Contour_Info> visualise_contours(const cv::Mat& src_frame, cv::Mat& draw_frame, int ContourMinSize,int ContourMaxSize, visualise_contours_mode mode)
std::vector<Contour_Info> visualise_contours(Pipeline_Frames& pf, const Trackbar_State& ts, Color_Config c, visualise_contours_mode mode){
  std::vector<Contour_Info> output;
  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(pf.morphology, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

  if (mode == NONE) return output;


  for (const auto& contour : contours) {

    std::vector<cv::Point> approx_contour;
    double epsilon = 0.02 * cv::arcLength(contour, true); // tolerance, scaled to contour size
    cv::approxPolyDP(contour, approx_contour, epsilon, true);


    double area = cv::contourArea(approx_contour);
    if (area < *ts.ContourMinSize) continue;
    if (area > *ts.ContourMaxSize) continue;

    cv::Moments m = cv::moments(approx_contour);
    if (m.m00 <= 0) continue;


    Contour_Info info;
    info.area = area;
    info.centroid = cv::Point2f((float)(m.m10 / m.m00), (float)(m.m01 / m.m00));
    info.bbox = cv::boundingRect(approx_contour);
    output.emplace_back(info);

    if (mode & BBOX){
      cv::rectangle(pf.original, info.bbox, cv::Scalar(0, 255, 0), 2);
    }
    if (mode & CENTROIDS){
      cv::circle(pf.original, info.centroid, 5, cv::Scalar(0, 0, 255), -1);
    }
    if (mode & CONTOURS){
      std::vector<std::vector<cv::Point>> single = {contour};
      cv::drawContours(pf.original, single, -1, cv::Scalar(255, 0, 0), 2);
      single = {approx_contour};
      cv::drawContours(pf.original, single, -1, cv::Scalar(255, 0, 0), 2);
    }
    if (mode & TEXT){
      cv::putText(pf.original, c.name, info.centroid, cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 255), 2);
    }
  }
  return output;
};


// cv::Mat& get_frame(cv::VideoCapture* p_cap){ };

int initialise_camera(cv::VideoCapture* p_cap){
  // Open the video camera.
  p_cap->open(pipeline, cv::CAP_GSTREAMER);
  if(!p_cap->isOpened()) {
    printf("Could not open camera.\n");
    return 1;
  }
  return 0;
};


// THIS IS TERRIBLY WRITTEN DONT TOUCH IT WORKS
// CHANGED: takes the brand's index into the shared classification map instead
// of re-running inRange() against the full HSV frame. pf->classification must
// already be populated (once per frame, before this loop) via
// classify_by_nearest_brand().
void process_contours(int color_idx, Pipeline_Frames *pf, Trackbar_State *tb, cv::MorphShapes ms) {
  pf->thresholded = (pf->classification == color_idx);
  pf->morphology = pf->thresholded.clone();
  if (*tb->iOpen) {
    cv::morphologyEx(pf->morphology, pf->morphology, cv::MORPH_OPEN, cv::getStructuringElement(ms, cv::Size(*tb->iOpen, *tb->iOpen)));
  }
  if (*tb->iClose) {
    cv::morphologyEx(pf->morphology, pf->morphology, cv::MORPH_CLOSE, cv::getStructuringElement(ms, cv::Size(*tb->iClose, (*tb->iClose)*7)));
  }

  cv::imshow("Camera - Morphology", pf->morphology);
  cv::waitKey(1);
};

using clk = std::chrono::steady_clock;
constexpr auto kPeriod = std::chrono::milliseconds(250);

int main(int argc, char* argv[])
{
  int program_mode = 0;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--debug") == 0) {
      program_mode = MODE_HEADLESS;
    }
    if (strcmp(argv[i], "--debug-camera") == 0){
      program_mode = MODE_DEBUG_CAMERA;
    }
    if (strcmp(argv[i], "--headless") == 0){
      program_mode = MODE_DEBUG_ALL;
    }
  }

  printf("debug_mode = %d\n", program_mode);

  // Measure the frame rate - initialise variables
  int frame_id = 0;
  timeval start, end;
  gettimeofday(&start, NULL);

  // Camera
#ifdef PI_CAMERA_MODULE
  cv::VideoCapture cap;
  if (initialise_camera(&cap) != 0){
    return 1;
  }
#endif

  // Create a control window
  cv::namedWindow("Control", cv::WINDOW_AUTOSIZE);
  int iLowH = Trackbar::LowH.default_val;
  int iHighH = Trackbar::HighH.default_val;
  int iLowS = Trackbar::LowS.default_val;
  int iHighS = Trackbar::HighS.default_val;
  int iLowV = Trackbar::LowV.default_val;
  int iHighV = Trackbar::HighV.default_val;
  // NOTE: Open/Close default to 0 in the trackbar panel, which means no
  // morphological cleanup runs until you manually drag both sliders up.
  // That is a large part of why the thresholded/morphology views in your
  // screenshots are ragged, text-shaped noise rather than solid can blobs.
  // Recommend changing the defaults in trackbar.h to something like
  // Open ~= 3 and Close ~= 5 so detection works without manual tuning on
  // startup; left as trackbar-driven here so you can still tune live.
  int iOpen = Trackbar::Open.default_val;
  int iClose = Trackbar::Close.default_val;
  int ContourMinSize = Trackbar::ContourMinArea.default_val;
  int ContourMaxSize = Trackbar::ContourMaxArea.default_val;

  Trackbar_State tb;
  tb.iLowH = &iLowH;
  tb.iHighH = &iHighH;
  tb.iLowS = &iLowS;
  tb.iHighS = &iHighS;
  tb.iLowV = &iLowV;
  tb.iHighV = &iHighV;
  tb.iOpen = &iOpen;
  tb.iClose = &iClose;
  tb.ContourMinSize = &ContourMinSize;
  tb.ContourMaxSize = &ContourMaxSize;

  cv::createTrackbar("LowH",  "Control", &iLowH,  Trackbar::LowH.max_val);
  cv::createTrackbar("HighH", "Control", &iHighH, Trackbar::HighH.max_val);
  cv::createTrackbar("LowS",  "Control", &iLowS,  Trackbar::LowS.max_val);
  cv::createTrackbar("HighS", "Control", &iHighS, Trackbar::HighS.max_val);
  cv::createTrackbar("LowV",  "Control", &iLowV,  Trackbar::LowV.max_val);
  cv::createTrackbar("HighV", "Control", &iHighV, Trackbar::HighV.max_val);
  cv::createTrackbar("Open",  "Control", &iOpen,  Trackbar::Open.max_val);
  cv::createTrackbar("Close", "Control", &iClose, Trackbar::Close.max_val);
  cv::createTrackbar("ContourMinArea", "Control", &ContourMinSize, Trackbar::ContourMinArea.max_val);
  cv::createTrackbar("ContourMaxArea", "Control", &ContourMaxSize, Trackbar::ContourMaxArea.max_val);

// std::vector<Color_Config> colors_vec = {
//   {"Coke",   'C', 3,  15, 190, 255, 130, 255},  // red
//   {"Fanta",  'F', 8,  20, 230, 255, 185, 205},  // orange — not yet sampled, placeholder
//   {"Sprite", 'G', 44, 60, 140, 255, 110, 255},  // green
//   {"Solo",   'S', 23, 35, 190, 255, 150, 255},  // yellow
// };
std::vector<Color_Config> colors_vec = {
  // {"Coke",   'C', 0,   6,  240, 255, 209, 240},  // red
  {"Coke",   'C', 0,   6,  220, 255, 160, 220},  // red
  {"Fanta",  'F', 3,  17,  236, 255, 130, 165},  // orange
  {"Mountain Dew", 'M', 41, 55,  133, 255, 66,  103},  // green — widened S ceiling to 255 for headroom
  {"Solo",   'S', 21, 34,  231, 255, 71,  103},  // yellow
};
  // NEW: precompute the brand HSV centres once. These drive the per-pixel
  // classification below and only need to be rebuilt if colors_vec changes.
  std::vector<Brand_Centre> brand_centres = build_brand_centres(colors_vec);

  Pipeline_Frames pf;
  cv::Moments Moment;

#ifndef PI_CAMERA_MODULE
  load_test_image(pf.bgr_img);
  pf.original = pf.bgr_img.clone();
#endif


  onMouse_UserData mouse_click_data;
  mouse_click_data.p_frame = &pf.original;

  // Create the OpenCV window
  cv::namedWindow("Camera", cv::WINDOW_NORMAL);
  cv::namedWindow("Camera - Thresholded", cv::WINDOW_NORMAL);
  cv::namedWindow("Camera - Morphology", cv::WINDOW_NORMAL);
#ifdef USE_FLAT_IMAGE
  cv::namedWindow("Camera - Flattend", cv::WINDOW_NORMAL);
#endif

  cv::setMouseCallback("Camera", onMouse, &mouse_click_data);

  auto next = clk::now();
  while(true) {

    next += kPeriod;

#ifdef PI_CAMERA_MODULE
    if (!cap.read(pf.original)) {
      printf("Could not read a frame.\n");
      return 1;
    }
#else
    pf.original = pf.bgr_img.clone();
#endif
    /* At this point in the code we have 'frame' regardless of NO_PI_CAMERA_MODULE flag */


#ifdef USE_FLAT_IMAGE
    pf.flattened = removeHighlightsShadows(pf.original);
    cv::cvtColor(pf.flattened, pf.hsv_frame, cv::COLOR_BGR2HSV);
#else
    cv::cvtColor(pf.original, pf.hsv_frame, cv::COLOR_BGR2HSV);
#endif

    if (mouse_click_data.just_clicked == true)
    {
      set_hsv_trackbars(mouse_click_data, tb);
      mouse_click_data.just_clicked = false;
    }

    // NEW: classify every pixel to its nearest brand ONCE per frame, before
    // the per-color loop, instead of re-thresholding the whole frame five
    // separate times. This is what stops adjacent hue bands (Coke/Sunkist/
    // Fanta/Solo) from all claiming the same pixel at once.
    pf.classification = classify_by_nearest_brand(pf.hsv_frame, brand_centres);

    std::vector<std::vector<Contour_Info>> img_info;
    for (size_t idx = 0; idx < colors_vec.size(); ++idx){
      const auto& color = colors_vec[idx];

      // process
      // store
      // repeat
      std::vector<Contour_Info> can_info;
      process_contours((int)idx, &pf, &tb, cv::MORPH_ELLIPSE);
      can_info = visualise_contours(pf, tb, color, ALL);
      img_info.push_back(can_info);
    }

    std::string packet = serialize_image(img_info, colors_vec);
    std::cout << packet.c_str() << std::endl;

    if (clk::now() > next) next = clk::now();  // fell behind: drop the beat, do not burst
    std::this_thread::sleep_until(next);

    // cv::putText(pf.original, "Sampling:  ", cv::Point2i(100,100) , cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 255), 2);
    // if (mouse_click_data.hsv_state.hsv_state == 0){
    //   cv::putText(pf.original, "Sampling: 1", cv::Point2i(100,100) , cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 255), 2);
    // }else{
    //   cv::putText(pf.original, "Sampling: 2", cv::Point2i(100,100) , cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 255), 2);
    // }

      show_all(pf, program_mode);
      cv::waitKey(1);

      // Measure the frame rate
      frame_id++;
      if (frame_id >= 30)
      {
        gettimeofday(&end, NULL);
        double diff = end.tv_sec - start.tv_sec + (end.tv_usec - start.tv_usec) / 1000000.0;
        printf("[FPS:%f] \n", 30 / diff);
        frame_id = 0;
        gettimeofday(&start, NULL);
      }
    }

#ifdef PI_CAMERA_MODULE
  // Free the camera 
  cap.release();
#endif
  return 0;
}
