#include "stdio.h"
#include "main.h"
#include "maix_util.hpp"
#include "maix_image.hpp"
#include "maix_time.hpp"
#include "maix_display.hpp"
#include "maix_rtsp.hpp"
#include "maix_camera.hpp"
#include "maix_basic.hpp"
#include <csignal>
#include <iostream>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include "socks.hpp"
#include "opencv2/opencv.hpp"
#include "opencv2/freetype.hpp"
#include <nlohmann/json.hpp>

using namespace std;
using namespace maix;
using json = nlohmann::json;

class OpticalFlowModule : public ConfigurableSocketModule {
public:
    OpticalFlowModule(const std::string& controller_ip, const std::string& module_name) :
        ConfigurableSocketModule(controller_ip, module_name), first_frame(true)
    {
    }

protected:
    void processor_run() override {
        Packet* packet = get_sub_data_packet("input_frame");
        if (!packet) {
            std::cerr << "Failed to get frame packet" << std::endl;
            return;
        }

        std::cout << "Delay_ns: " << get_uptime_nanoseconds() - packet->emit_time_ns << std::endl;

        cv::Mat img_maix = Packet::packet_to_cv_mat(packet);
        
        // Convert to grayscale
        cv::Mat gray;
        cv::cvtColor(img_maix, gray, cv::COLOR_BGR2GRAY);
        
        if (first_frame) {
            prev_gray = gray.clone();
            first_frame = false;
            delete packet;
            return;
        }
        
        // Calculate optical flow
        cv::Mat flow;
        cv::calcOpticalFlowFarneback(
            prev_gray, gray, flow,
            0.5,              // pyr_scale
            2,                // levels (was 3)
            5,                // winsize (was 15)
            2,                // iterations (was 3)
            5,                // poly_n
            1.1,              // poly_sigma
            cv::OPTFLOW_FARNEBACK_GAUSSIAN // faster than polynomial
        );
        
        // Split flow into x and y components
        std::vector<cv::Mat> flow_planes(2);
        cv::split(flow, flow_planes);
        cv::Mat flow_x = flow_planes[0], flow_y = flow_planes[1];
        
        // Calculate magnitude and angle
        cv::Mat magnitude, angle;
        cv::cartToPolar(flow_x, flow_y, magnitude, angle, true);
        
        // Convert angle to 0-180 for HSV Hue
        angle *= (1.0 / 2.0);
        
        // Normalize magnitude to 0-255
        cv::Mat mag_norm;
        cv::normalize(magnitude, mag_norm, 0, 255, cv::NORM_MINMAX, CV_8UC1);
        
        // Create HSV image
        std::vector<cv::Mat> hsv_planes = {
            angle, 
            cv::Mat_<uchar>::ones(angle.size()) * 255, // Saturation
            mag_norm // Value
        };
        
        cv::Mat hsv_image;
        cv::merge(hsv_planes, hsv_image);
        
        // Convert HSV to BGR
        cv::Mat flow_bgr;
        cv::cvtColor(hsv_image, flow_bgr, cv::COLOR_HSV2BGR);
        
        // Convert to maix image (assuming BGR format)
        maix::image::Image* flow_img = new image::Image(
            flow_bgr.cols, flow_bgr.rows, 
            image::FMT_BGR888,
            flow_bgr.data, flow_bgr.total() * flow_bgr.elemSize(),
            true
        );
        
        // Create output packet
        Packet* low_res_packet = Packet::maix_image_to_packet(flow_img, packet->emit_time_ns);
        
        // Send result
        set_pub_data_packet("output_frame", low_res_packet);
        
        // Clean up
        prev_gray = gray.clone();
        delete flow_img;  // Only safe if Packet makes a deep copy
        delete packet;
    }

private:
    cv::Mat prev_gray;
    bool first_frame;
};

int _main(int argc, char* argv[])
{
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <controller_ip> <module_name>" << std::endl;
        return 1;
    }
    
    std::string controller_ip = argv[1];
    std::string module_name = argv[2];
    
    OpticalFlowModule opticalFlowModule(controller_ip, module_name);
    opticalFlowModule.start();

    while(!app::need_exit()) {
        time::sleep_ms(1000);
    }
    opticalFlowModule.stop();

    return 0;
}

int main(int argc, char* argv[])
{
    sys::register_default_signal_handle();
    CATCH_EXCEPTION_RUN_RETURN(_main, -1, argc, argv);
}