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
// #include "opencv2/aruco.hpp"
#include <nlohmann/json.hpp>

using namespace std;
using namespace maix;
using json = nlohmann::json;

class ArucoModule : public ConfigurableSocketModule {
public:
    ArucoModule(const std::string& controller_ip, const std::string& module_name) :
        ConfigurableSocketModule(controller_ip, module_name)
        // Явное создание объекта Dictionary через new
        // dictionary(new cv::aruco::Dictionary(cv::aruco::getPredefinedDictionary(cv::aruco::DICT_6X6_250))),
        // // Явное создание объекта DetectorParameters через new
        // detectorParams(new cv::aruco::DetectorParameters())
    {
        // Здесь можно инициализировать параметры ArUco, если нужно
    }

protected:
    void processor_run() override {
        std::cout << "Trying to recieve packet" << std::endl;
        Packet* packet = get_sub_data_packet("input_frame");
        std::cout << "Trying to recieved packet" << std::endl;
        if (!packet) {
            std::cerr << "Failed to get frame packet" << std::endl;
            return;
        }

        std::cout << "Delay_ns: " << get_uptime_nanoseconds() - packet->emit_time_ns << std::endl;

        cv::Mat img_maix = Packet::packet_to_cv_mat(packet);

        // std::vector<std::vector<cv::Point2f>> corners, rejected;
        // std::vector<int> ids;

        // Детекция маркеров ArUco
        // cv::aruco::detectMarkers(img_maix, dictionary, corners, ids, detectorParams, rejected);

        // json detected_markers;

        // // Записываем ids
        // detected_markers["ids"] = json::array();
        // for (int id : ids) {
        //     detected_markers["ids"].push_back(id);
        // }

        // // Записываем углы
        // detected_markers["corners"] = json::array();
        // for (const auto& corner : corners) {
        //     json corner_array = json::array();
        //     for (const auto& point : corner) {
        //         json pt = json::array();
        //         pt.push_back(point.x);
        //         pt.push_back(point.y);
        //         corner_array.push_back(pt);
        //     }
        //     detected_markers["corners"].push_back(corner_array);
        // }

        // // Записываем rejected
        // detected_markers["rejected"] = json::array();
        // for (const auto& rej : rejected) {
        //     json rej_array = json::array();
        //     for (const auto& point : rej) {
        //         json pt = json::array();
        //         pt.push_back(point.x);
        //         pt.push_back(point.y);
        //         rej_array.push_back(pt);
        //     }
        //     detected_markers["rejected"].push_back(rej_array);
        // }

        // // Создаём выходной пакет
        // Packet* output_packet = new Packet(detected_markers.dump(), get_uptime_nanoseconds());
        // set_pub_data_packet("detected_markers", output_packet);

        // Очистка
        delete packet;
    }

// private:
//     cv::Ptr<cv::aruco::Dictionary> dictionary;
//     cv::Ptr<cv::aruco::DetectorParameters> detectorParams;
};

int _main(int argc, char* argv[])
{
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <controller_ip> <module_name>" << std::endl;
        return 1;
    }
    
    std::string controller_ip = argv[1];
    std::string module_name = argv[2];
    
    ArucoModule arucoModule(controller_ip, module_name);
    arucoModule.start();

    std::cin.get(); // keep running
    arucoModule.stop();

    return 0;
}

int main(int argc, char* argv[])
{
    sys::register_default_signal_handle();
    CATCH_EXCEPTION_RUN_RETURN(_main, -1, argc, argv);
}
