/* 
Requires OpenCV 4.x - OpenCV 5.0.0 (Homebrew) doesn't work as of July 2026 missing moments class
OpenCV 4.x Homebrew or windows equivelant.
*/

/*TODO
update README
Can distance thresholding
report multiple distinct colors of cans
*/

#define TEST

#include <opencv2/opencv.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <sys/time.h>

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

//Claude wrote this:
void onMouse(int event, int x, int y, int flags, void* userdata) {
    cv::Mat* img = reinterpret_cast<cv::Mat*>(userdata);
    if (event == cv::EVENT_LBUTTONDOWN) {
        cv::Vec3b bgr = img->at<cv::Vec3b>(y, x); // note: row=y, col=x
        printf("Clicked (%d, %d) -> BGR: (%d, %d, %d)\n", x, y, bgr[0], bgr[1], bgr[2]);

        cv::Mat hsvPixel;
        cv::cvtColor(cv::Mat(1, 1, CV_8UC3, bgr), hsvPixel, cv::COLOR_BGR2HSV);
        cv::Vec3b hsv = hsvPixel.at<cv::Vec3b>(0, 0);
        printf("HSV: (%d, %d, %d)\n", hsv[0], hsv[1], hsv[2]);
    }
};

// Splits touching blobs via watershed and returns one Contour_Info per
// segmented region (a single isolated can still returns exactly one entry).
std::vector<Contour_Info> visualise_contours_watershed(cv::Mat& draw_frame, const cv::Mat& morph_frame,
                                                         int ContourMinSize, double peakThreshold,
                                                         visualise_contours_mode mode) {
    std::vector<Contour_Info> output;
    if (mode == NONE) return output;

    // 1. Distance transform across the whole mask
    cv::Mat dist;
    cv::distanceTransform(morph_frame, dist, cv::DIST_L2, 5);
    cv::normalize(dist, dist, 0, 1.0, cv::NORM_MINMAX);

    // 2. Sure foreground = distance-transform peaks (can cores)
    cv::Mat sureFg;
    cv::threshold(dist, sureFg, peakThreshold, 1.0, cv::THRESH_BINARY);
    sureFg.convertTo(sureFg, CV_8U, 255);

    // 3. Sure background = dilated mask
    cv::Mat sureBg;
    cv::dilate(morph_frame, sureBg, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5)), cv::Point(-1, -1), 3);

    // 4. Unknown = area between sure foreground and sure background
    cv::Mat unknown;
    cv::subtract(sureBg, sureFg, unknown);

    // 5. Label each foreground blob as its own marker/seed
    cv::Mat markers;
    int numMarkers = cv::connectedComponents(sureFg, markers);
    markers = markers + 1;          // background becomes 1, not 0
    markers.setTo(0, unknown == 255); // 0 = unknown, watershed will resolve it

    // 6. Watershed needs a 3-channel image to run against
    cv::Mat frame3ch;
    if (draw_frame.channels() == 1) cv::cvtColor(draw_frame, frame3ch, cv::COLOR_GRAY2BGR);
    else frame3ch = draw_frame;

    markers.convertTo(markers, CV_32S);
    cv::watershed(frame3ch, markers);
    // markers now holds: -1 = boundary line, 1 = background, 2..N = each can

    // 7. Extract one Contour_Info per segmented region
    for (int label = 2; label < numMarkers + 1; ++label) {
        cv::Mat regionMask = (markers == label);
        regionMask.convertTo(regionMask, CV_8U, 255);

        double area = cv::countNonZero(regionMask);
        if (area < ContourMinSize) continue;

        cv::Moments m = cv::moments(regionMask, true);
        if (m.m00 <= 0) continue;

        Contour_Info info;
        info.area = area;
        info.centroid = cv::Point2f((float)(m.m10 / m.m00), (float)(m.m01 / m.m00));
        info.bbox = cv::boundingRect(regionMask);
        output.emplace_back(info);

        if (mode & BBOX) {
            cv::rectangle(draw_frame, info.bbox, cv::Scalar(0, 255, 0), 2);
        }
        if (mode & CENTROIDS) {
            cv::circle(draw_frame, info.centroid, 5, cv::Scalar(0, 0, 255), -1);
        }
        if (mode & CONTOURS) {
            std::vector<std::vector<cv::Point>> regionContours;
            cv::findContours(regionMask, regionContours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
            cv::drawContours(draw_frame, regionContours, -1, cv::Scalar(255, 0, 0), 2);
        }
    }

    // Draw the watershed split lines themselves, useful for tuning peakThreshold
    if (mode & CONTOURS) {
        draw_frame.setTo(cv::Scalar(0, 255, 255), markers == -1);
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
    int iPeakThreshold = 50; // 0-100, represents 0.00-1.00 for the distance transform cutoff

    cv::createTrackbar("LowH",  "Control", &iLowH,  Trackbar::LowH.max_val);
    cv::createTrackbar("HighH", "Control", &iHighH, Trackbar::HighH.max_val);
    cv::createTrackbar("LowS",  "Control", &iLowS,  Trackbar::LowS.max_val);
    cv::createTrackbar("HighS", "Control", &iHighS, Trackbar::HighS.max_val);
    cv::createTrackbar("LowV",  "Control", &iLowV,  Trackbar::LowV.max_val);
    cv::createTrackbar("HighV", "Control", &iHighV, Trackbar::HighV.max_val);
    cv::createTrackbar("Open",  "Control", &iOpen,  Trackbar::Open.max_val);
    cv::createTrackbar("Close", "Control", &iClose, Trackbar::Close.max_val);
    cv::createTrackbar("ContourMinArea", "Control", &ContourMinSize, Trackbar::ContourMinArea.max_val);
    cv::createTrackbar("PeakThreshold (%)", "Control", &iPeakThreshold, 100);

    // Measure the frame rate - initialise variables
    int frame_id = 0;
    timeval start, end;
    gettimeofday(&start, NULL);

    cv::Mat frame;
    cv::Mat hsv_frame;
    cv::Mat thresh_frame;
    cv::Mat morph_frame;

#ifdef TEST
    cv::Mat bgr_img;
    load_test_image(bgr_img);
    frame = bgr_img.clone();
#endif

    // Create the OpenCV windows
    cv::namedWindow("Camera", cv::WINDOW_NORMAL);
    cv::setMouseCallback("Camera", onMouse, &frame);
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

        cv::cvtColor(frame, hsv_frame, cv::COLOR_BGR2HSV);

        inRange(hsv_frame, cv::Scalar(iLowH, iLowS, iLowV), cv::Scalar(iHighH, iHighS, iHighV), thresh_frame);
        morph_frame = thresh_frame.clone();
        if (iOpen) {
            cv::morphologyEx(morph_frame, morph_frame, cv::MORPH_OPEN, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(iOpen, iOpen)));
        }
        if (iClose) {
            cv::morphologyEx(morph_frame, morph_frame, cv::MORPH_CLOSE, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(iClose, iClose)));
        }

        //need to add can propoties. the region is taller than it is wide.

        double peakThreshold = iPeakThreshold / 100.0;
        std::vector<Contour_Info> img_info =
            visualise_contours_watershed(frame, morph_frame, ContourMinSize, peakThreshold, ALL);

        //show frame
        cv::imshow("Camera", frame);
        cv::imshow("Camera - Thresholded", thresh_frame);
        cv::imshow("Camera - Morphology", morph_frame);
        cv::waitKey(1);

        // Measure the frame rate
        frame_id++;
        if (frame_id >= 30) {
            printf("Number of Cans: %zu\n", img_info.size());
            gettimeofday(&end, NULL);
            double diff = end.tv_sec - start.tv_sec + (end.tv_usec - start.tv_usec)/1000000.0;
            printf("30 frames in %f seconds = %f FPS\n", diff, 30/diff);
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
