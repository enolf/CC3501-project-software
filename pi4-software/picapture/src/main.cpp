/* 
Requires OpenCV 4.x - OpenCV 5.0.0 (Homebrew) doesn't work as of July 2026 missing moments class
OpenCV 4.x Homebrew or windows equivelant.
*/

#define TEST

#include <opencv2/opencv.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <sys/time.h>

#ifdef TEST
int load_test_image(cv::Mat &bgr_out) {
    // Load the image
    bgr_out = cv::imread("../img.jpg");
    if (!bgr_out.data) {
        bgr_out = cv::imread("img.jpg");
        if (!bgr_out.data) {
            std::cerr << "Failed to load image\n";
            return 1;
        }
    }
    return 0;
};

#endif

int main()
{
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
    

    // Create a control window
    cv::namedWindow("Control", cv::WINDOW_AUTOSIZE);
    int iLowH = 0;
    int iHighH = 179;

    int iLowS = 0;
    int iHighS = 255;

    int iLowV = 0;
    int iHighV = 255;

    int iOpen = 0;
    int iClose = 0;
 
    // Create trackbars in "Control" window
    cv::createTrackbar("LowH", "Control", &iLowH, 179); //Hue (0 - 179)
    cv::createTrackbar("HighH", "Control", &iHighH, 179);

    cv::createTrackbar("LowS", "Control", &iLowS, 255); //Saturation (0 - 255)
    cv::createTrackbar("HighS", "Control", &iHighS, 255);

    cv::createTrackbar("LowV", "Control", &iLowV, 255); //Value (0 - 255)
    cv::createTrackbar("HighV", "Control", &iHighV, 255);

    cv::createTrackbar("Open", "Control", &iOpen, 10); //Value (0 - 10)
    cv::createTrackbar("Close", "Control", &iClose, 10); //Value (0 - 10)

    // Create the OpenCV window
    cv::namedWindow("Camera", cv::WINDOW_AUTOSIZE);
    cv::namedWindow("Camera - Thresholded", cv::WINDOW_AUTOSIZE);
    cv::namedWindow("Camera - Morphology", cv::WINDOW_AUTOSIZE);

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
    for(;;) {
#ifndef TEST
        if (!cap.read(frame)) {
            printf("Could not read a frame.\n");
            break;
        }
#endif

	cv::cvtColor(frame, hsv_frame, cv::COLOR_BGR2HSV);

	hsv_frame =	thresh_frame.clone();

	// Threshold the image
	// might need to cvtColor this frame first
	inRange(hsv_frame, cv::Scalar(iLowH, iLowS, iLowV), cv::Scalar(iHighH, iHighS, iHighV), thresh_frame);
	morph_frame = thresh_frame;
	if (iOpen){
		cv::morphologyEx(morph_frame, morph_frame, cv::MORPH_OPEN, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(iOpen, iOpen)));
	}
	if (iClose){
		cv::morphologyEx(morph_frame, morph_frame, cv::MORPH_CLOSE, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(iClose, iClose)));
	}

	Moment = cv::moments(morph_frame, true);
	if (Moment.m00 > 0){
		double cx = Moment.m10 / Moment.m00;
		double cy = Moment.m01 / Moment.m00;
		cv::circle(frame, cv::Point((int)cx, (int)cy), 5, cv::Scalar(0, 0, 255), -1);
		printf("Centroid: (%.1f, %.1f)  Area: %.1f\n", cx, cy, Moment.m00);
	}

        //show frame
        cv::imshow("Camera", frame);
        cv::imshow("Camera - Thresholded", thresh_frame);
        cv::imshow("Camera - Morphology", morph_frame);
        cv::waitKey(1);

        // Measure the frame rate
        frame_id++;
        if (frame_id >= 30) {
            gettimeofday(&end, NULL);
            double diff = end.tv_sec - start.tv_sec + (end.tv_usec - start.tv_usec)/1000000.0;
            printf("30 frames in %f seconds = %f FPS\n", diff, 30/diff);
            frame_id = 0;
            gettimeofday(&start, NULL);
        }
    }

    // Free the camera 
    cap.release();
    return 0;
}

