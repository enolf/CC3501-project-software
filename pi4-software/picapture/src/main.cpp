/* 
Requires OpenCV 4.x - OpenCV 5.0.0 (Homebrew) doesn't work as of July 2026 missing moments class
OpenCV 4.x Homebrew or windows equivelant.
*/

/*TODO
update README
Can distance thresholding
report multiple distinct colors of cans
*/

// #define TEST

#include <opencv2/opencv.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <sys/time.h>
#include <string>
#include <sstream>

#include "trackbar.h"

enum visualise_contours_mode {
    NONE = 0,
    BBOX = 1 << 0,
    CENTROIDS = 1 << 1,
    CONTOURS = 1 << 2,
    ALL = BBOX | CENTROIDS | CONTOURS
};

struct Contour_Info {
    cv::Rect bbox;
    cv::Point2f centroid;
    double area;
    cv::Vec3b hsv;
    std::string color;
};

struct onMouse_UserData{
    bool just_clicked = false;
    cv::Mat* p_frame;
    cv::Vec3b hsv;
};

struct Trackbar_State{
    int* iLowH;
    int* iHighH;
    int* iLowS;
    int* iHighS;
    int* iLowV;
    int* iHighV;
};



#ifdef TEST
int load_test_image(cv::Mat &bgr_out) {
    // Load the image
    bgr_out = cv::imread("../cans_test/cans_multi_test_1.jpg");
    if (!bgr_out.data) {
        bgr_out = cv::imread("cans_test/cans_multi_test_1.jpg");
        if (!bgr_out.data) {
            std::cerr << "Failed to load image\n";
            return 1;
        }
    }
    return 0;
};
#endif

std::string serialize_image(const std::vector<std::vector<Contour_Info>>& image_info, const std::vector<std::string>& can_color_names){
    std::ostringstream oss;
    if (can_color_names.size() != image_info.size() && ""){
        printf("length of can_color_name and image_info don't match");
        return "error in serialize_image";
    }
    for (int i = 0; i < image_info.size(); i++){
        oss << can_color_names[i] << ": " << image_info[i].size();
        oss << "; ";
    }
    oss << "\n";
    return oss.str();
};

//Claude wrote the base of this function:
void onMouse(int event, int x, int y, int flags, void* userdata) {
    onMouse_UserData* data = reinterpret_cast<onMouse_UserData*>(userdata);


    if (event == cv::EVENT_LBUTTONDOWN) {
        cv::Vec3b bgr = data->p_frame->at<cv::Vec3b>(y, x); // note: row=y, col=x
        printf("Clicked (%d, %d) -> BGR: (%d, %d, %d)\n", x, y, bgr[0], bgr[1], bgr[2]);

        // If you also want HSV at that point:
        cv::Mat hsvPixel;
        cv::cvtColor(cv::Mat(1, 1, CV_8UC3, bgr), hsvPixel, cv::COLOR_BGR2HSV);
        data->hsv = hsvPixel.at<cv::Vec3b>(0, 0);
        printf("HSV: (%d, %d, %d)\n", data->hsv[0], data->hsv[1], data->hsv[2]);
    }
};

void set_hsv_trackbars(const onMouse_UserData& data, Trackbar_State& tb){
    cv::Vec3b hsv = data.hsv;
    int tolerance = 10;
    int bigger_tolerance = 20;

    *tb.iLowH  = std::max(0, hsv[0] - tolerance);
    *tb.iHighH = std::min(179, hsv[0] + tolerance);
    *tb.iLowS  = std::max(0, hsv[1] - bigger_tolerance);
    *tb.iHighS = std::min(255, hsv[1] + bigger_tolerance);
    *tb.iLowV  = std::max(0, hsv[2] - bigger_tolerance);
    *tb.iHighV = std::min(255, hsv[2] + bigger_tolerance);

    cv::setTrackbarPos("LowH", "Control", *tb.iLowH);
    cv::setTrackbarPos("HighH", "Control", *tb.iHighH);
    cv::setTrackbarPos("LowS",  "Control", *tb.iLowS);
    cv::setTrackbarPos("HighS", "Control", *tb.iHighS);
    cv::setTrackbarPos("LowV",  "Control", *tb.iLowV);
    cv::setTrackbarPos("HighV", "Control", *tb.iHighV);
}

std::vector<Contour_Info> visualise_contours(const cv::Mat& src_frame, cv::Mat& draw_frame, int ContourMinSize,int ContourMaxSize, visualise_contours_mode mode){
    std::vector<Contour_Info> output;
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(src_frame, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    if (mode == NONE) return output;

    for (const auto& contour : contours) {
        double area = cv::contourArea(contour);
        if (area < ContourMinSize) continue;
        if (area > ContourMaxSize) continue;

        cv::Moments m = cv::moments(contour);
        if (m.m00 <= 0) continue;

        Contour_Info info;
        info.area = area;
        info.centroid = cv::Point2f((float)(m.m10 / m.m00), (float)(m.m01 / m.m00));
        info.bbox = cv::boundingRect(contour);
        output.emplace_back(info);

        if (mode & BBOX){
            cv::rectangle(draw_frame, info.bbox, cv::Scalar(0, 255, 0), 2);
        }
        if (mode & CENTROIDS){
            cv::circle(draw_frame, info.centroid, 5, cv::Scalar(0, 0, 255), -1);
        }
        if (mode & CONTOURS){
            std::vector<std::vector<cv::Point>> single = {contour};
            cv::drawContours(draw_frame, single, -1, cv::Scalar(255, 0, 0), 2);
        }
    }
    return output;
};


int main()
{
    #ifndef TEST
    // Open the video camera.
    std::string pipeline = "libcamerasrc"
        " ! video/x-raw, width=800, height=600" // camera needs to capture at a higher resolution
        " ! videoconvert"
        " ! videoscale"
        " ! video/x-raw, width=400, height=300" // can downsample the image after capturing
        " ! videoflip method=rotate-180" // remove this line if the image is upside-down
        " ! appsink drop=true max_buffers=2";
    cv::VideoCapture cap(pipeline, cv::CAP_GSTREAMER);
    if(!cap.isOpened()) {
        printf("Could not open camera.\n");
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

    // Measure the frame rate - initialise variables
    int frame_id = 0;
    timeval start, end;
    gettimeofday(&start, NULL);

    cv::Mat frame;
    cv::Mat hsv_frame;
    cv::Mat thresh_frame;
    cv::Mat morph_frame;
    cv::Moments Moment;

#ifdef TEST
    cv::Mat bgr_img;
    load_test_image(bgr_img);
    frame = bgr_img.clone();
#endif

    onMouse_UserData mouse_click_data;
    mouse_click_data.p_frame = &frame;

    // Create the OpenCV window
    cv::namedWindow("Camera", cv::WINDOW_NORMAL);
    cv::setMouseCallback("Camera", onMouse, &mouse_click_data);
    cv::namedWindow("Camera - Thresholded", cv::WINDOW_NORMAL);
    cv::namedWindow("Camera - Morphology", cv::WINDOW_NORMAL);

    while(true) {
#ifndef TEST
        if (!cap.read(frame)) {
            printf("Could not read a frame.\n");
            break;
        }
#else
    frame = bgr_img.clone();
#endif

        
        set_hsv_trackbars(mouse_click_data, tb);

        cv::cvtColor(frame, hsv_frame, cv::COLOR_BGR2HSV);

        // hsv_frame =	thresh_frame.clone();

        // Threshold the image
        // might need to cvtColor this frame first
        inRange(hsv_frame, cv::Scalar(iLowH, iLowS, iLowV), cv::Scalar(iHighH, iHighS, iHighV), thresh_frame);
        morph_frame = thresh_frame.clone();
        if (iOpen){
            cv::morphologyEx(morph_frame, morph_frame, cv::MORPH_OPEN, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(iOpen, iOpen)));
        }
        if (iClose){
            cv::morphologyEx(morph_frame, morph_frame, cv::MORPH_CLOSE, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(iClose, iClose)));
        }


        //need to add can propoties. the region is taller than it is wide.

        std::vector<std::vector<Contour_Info>> img_info;
        std::vector<Contour_Info> can_info;
        // std::vector<std::string> can_color_names = {"Coke", "Pepsi", "Fanta"};
        std::vector<std::string> can_color_names = {"COKE"};

        can_info = visualise_contours(morph_frame, frame, ContourMinSize, ContourMaxSize, ALL);
        //red
        can_info = visualise_contours(morph_frame, frame, ContourMinSize, ContourMaxSize, ALL);
        //blue
        can_info = visualise_contours(morph_frame, frame, ContourMinSize, ContourMaxSize, ALL);
        // purple
        can_info = visualise_contours(morph_frame, frame, ContourMinSize, ContourMaxSize, ALL);

        img_info.push_back(can_info);

        std::string packet = serialize_image(img_info, can_color_names);
        //print
        //flush

        //show frame
        cv::imshow("Camera", frame);
        cv::imshow("Camera - Thresholded", thresh_frame);
        cv::imshow("Camera - Morphology", morph_frame);
        cv::waitKey(1);

        // Measure the frame rate
        frame_id++;
        if (frame_id >= 30) {
            printf("Packet for sending \"%s\"", packet.c_str());
            gettimeofday(&end, NULL);
            double diff = end.tv_sec - start.tv_sec + (end.tv_usec - start.tv_usec)/1000000.0;
            printf("[FPS: %f] Number of Cans: %zu\n", 30/diff, can_info.size());
            // printf("30 frames in %f seconds = %f FPS\n", diff, 30/diff);
            frame_id = 0;
            gettimeofday(&start, NULL);
        }
    }

    #ifndef TEST
    // Free the camera 
    cap.release();
    #endif
    return 0;
}

