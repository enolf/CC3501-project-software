/* 
Requires OpenCV 4.x - OpenCV 5.0.0 (Homebrew) doesn't work as of July 2026 missing moments class
OpenCV 4.x Homebrew or windows equivelant.
*/

/*TODO
update README
Can distance thresholding
report multiple distinct colors of cans
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

#include "trackbar.h"

// #define USE_FLAT_IMAGE
#define TESTING
// #define PI_CAMERA_MODULE

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
#ifdef USE_FLAT_IMAGE
  cv::Mat flattened;
#endif
};

constexpr const char* PATH_TO_IMAGE = "cans_test/cans_test_4.jpg";
constexpr const char* BACKUP_PATH_TO_IMAGE = "../cans_test/cans_test_4.jpg";

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
  cv::Mat base;
  bgr_input.convertTo(base, CV_32F, 1.0 / 255.0);

  cv::Mat gray;
  cv::cvtColor(bgr_input, gray, cv::COLOR_BGR2GRAY);

  // Blur BEFORE inverting — this is the.c_str() step that was missing.
  // It's what turns "harsh local contrast" into "smooth lighting removal".
  int k = blurRadius | 1;  // GaussianBlur requires an odd kernel size
  cv::Mat blurred;
  cv::GaussianBlur(gray, blurred, cv::Size(k, k), 0);

  cv::Mat blurredF;
  blurred.convertTo(blurredF, CV_32F, 1.0 / 255.0);
  cv::Mat inverted = 1.0f - blurredF;

  cv::Mat blend;
  cv::cvtColor(inverted, blend, cv::COLOR_GRAY2BGR);

  // Single soft-light application — no iteration
  cv::Mat base2, sqrtBase;
  cv::multiply(base, base, base2);
  cv::sqrt(base, sqrtBase);

  cv::Mat lowResult  = 2 * base.mul(blend) + base2.mul(1.0 - 2 * blend);
  cv::Mat highResult = 2 * base.mul(1.0 - blend) + sqrtBase.mul(2 * blend - 1.0);

  std::vector<cv::Mat> blendChannels;
  cv::split(blend, blendChannels);
  cv::Mat mask1c;
  cv::compare(blendChannels[0], 0.5, mask1c, cv::CMP_LE);

  cv::Mat result = base.clone();
  lowResult.copyTo(result, mask1c);
  highResult.copyTo(result, ~mask1c);

  cv::Mat output;
  result.convertTo(output, CV_8U, 255.0);
  return output;
}
#endif
void show_all(const Pipeline_Frames& pf){
  cv::imshow("Camera", pf.original);
  // cv::imshow("Camera - Thresholded", pf.thresholded);
  cv::imshow("Camera - Morphology", pf.morphology);
#ifdef USE_FLAT_IMAGE
  cv::imshow("Camera - Flattend", flat_frame);
#endif
}
// void show_all(const std::vector<cv::Mat>& fs){
//         for (const auto& f : fs){ cv::imshow("", f);}
// }

std::string serialize_image(const std::vector<std::vector<Contour_Info>>& image_info, const std::vector<Color_Config>& color_config){
  size_t name_idx = 0;
  std::ostringstream oss;
  for (int i = 0; i < image_info.size(); i++){
    if (i != 0){ oss << ","; }
    oss << (color_config[i].name)[0] << ":" << image_info[i].size();
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
void process_contours(const Color_Config& color, Pipeline_Frames *pf, Trackbar_State *tb, cv::MorphShapes ms) {
  // printf("Processing Contours for %s\n", color.name.c_str());
  // *tb->iLowH = color.lowH;
  // *tb->iLowS = color.lowS;
  // *tb->iLowV = color.lowV;
  // *tb->iHighH = color.highH;
  // *tb->iHighS = color.highS;
  // *tb->iHighV = color.highV;
  // tb->has_changed = true;


  inRange(pf->hsv_frame, cv::Scalar(color.lowH, color.lowS, color.lowV),
          cv::Scalar(color.highH, color.highS, color.highV), pf->thresholded);
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

int main()
{

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
  //     {"Sunkist", 5, 20, 150, 200, 150, 225},     // red
  //     {"Solo", 15, 30, 200, 255, 150, 200},     // red
  //     {"Fanta", 10, 20, 150, 200, 180, 215},   // orange
  //     {"Coke", 170, 179, 200, 255, 110, 150},    // yellow
  //     {"Pepsi", 90, 110, 140, 155, 220, 240}, // purple
  // };
  std::vector<Color_Config> colors_vec = {
    {"Sunkist", 9, 16, 160, 255, 150, 255},    // orange
    {"Solo", 25, 34, 150, 255, 150, 255},    // yellow
    {"Fanta", 17, 24, 180, 255, 150, 255}, // deeper orange, higher sat than Fanta
    {"Coke", 0, 8, 160, 255, 100, 255},      // red
    {"Pepsi", 100, 120, 150, 255, 100, 255}, // blue
  };
  // std::vector<Color_Config> colors_vec = {
  //     {"Coke", 0, 10, 150, 255, 100, 255},     // red
  //     {"Fanta", 10, 25, 150, 255, 100, 255},   // orange
  //     {"Solo", 25, 35, 120, 255, 150, 255},    // yellow
  //     {"Passito", 130, 160, 60, 255, 60, 255}, // purple
  // };

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
    if (!cap.read(frame)) {
      printf("Could not read a frame.\n");
      return 1;
    }
#else
    pf.original = pf.bgr_img.clone();
#endif
    /* At this point in the code we have 'frame' regardless of NO_PI_CAMERA_MODULE flag */

#ifdef USE_FLAT_IMAGE
    flat_frame = removeHighlightsShadows(frame);
    cv::cvtColor(pf.flattened, pf.hsv_frame, cv::COLOR_BGR2HSV);
#else
    cv::cvtColor(pf.original, pf.hsv_frame, cv::COLOR_BGR2HSV);
#endif

    std::vector<std::vector<Contour_Info>> img_info;
    for (const auto& color : colors_vec){

      if (mouse_click_data.just_clicked == true)
      {
        set_hsv_trackbars(mouse_click_data, tb);
        mouse_click_data.just_clicked = false;
      }
      // process
      // store
      // repeat
      std::vector<Contour_Info> can_info;
      process_contours(color, &pf, &tb, cv::MORPH_ELLIPSE);
      can_info = visualise_contours(pf, tb, color, ALL);
      img_info.push_back(can_info);
    }

    std::string packet = serialize_image(img_info, colors_vec);
    std::cout << packet.c_str() << std::endl;
    //print
    //flush
    if (clk::now() > next) next = clk::now();  // fell behind: drop the beat, do not burst
    std::this_thread::sleep_until(next);

    cv::putText(pf.original, "Sampling:  ", cv::Point2i(100,100) , cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 255), 2);
    if (mouse_click_data.hsv_state.hsv_state == 0){
      cv::putText(pf.original, "Sampling: 1", cv::Point2i(100,100) , cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 255), 2);
    }else{
      cv::putText(pf.original, "Sampling: 2", cv::Point2i(100,100) , cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 255), 2);
    } 
    show_all(pf);
    cv::waitKey(1);

    // Measure the frame rate
    frame_id++;
    if (frame_id >= 30) {
      gettimeofday(&end, NULL);
      double diff = end.tv_sec - start.tv_sec + (end.tv_usec - start.tv_usec)/1000000.0;
      // printf("[FPS:%f] Packet for sending: \"%s\"\n", 30/diff ,packet.c_str());
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

