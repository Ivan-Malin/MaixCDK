#include "stdio.h"
#include "main.h"
#include "maix_util.hpp"
#include "maix_image.hpp"
#include "maix_time.hpp"
#include "maix_display.hpp"
#include "maix_rtsp.hpp"
#include "maix_camera.hpp"
#include "maix_basic.hpp"
#include "csignal"
#include <iostream>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include "socks.hpp"
#include "opencv2/opencv.hpp"
#include "opencv2/freetype.hpp"
#include "opencv2/aruco.hpp"
#include <nlohmann/json.hpp>  // или #include <nlohmann/json.hpp>, в зависимости от используемой библиотеки JSON
#include <iostream>

using namespace std;
using namespace maix;

class ArucoModule : public ConfigurableSocketModule {
public:
    ArucoModule(const std::string& controller_ip, const std::string& module_name) :
        ConfigurableSocketModule(controller_ip, module_name)
    {
        // Здесь можно инициализировать параметры ArUco, если нужно
        dictionary = cv::aruco::getPredefinedDictionary(cv::aruco::DICT_6X6_250);
    }

protected:
    void processor_run() override {
        // Получаем пакет с изображением
        Packet* packet = get_sub_data_packet("frame");
        if (!packet) {
            std::cerr << "Failed to get frame packet" << std::endl;
            return;
        }

        // Время задержки
        std::cout << "Delay_ns: " << get_uptime_nanoseconds() - packet->emit_time_ns << std::endl;

        // Конвертируем в cv::Mat
        cv::Mat img_maix = packet_to_cv_mat(packet);

        // Результаты детекции
        std::vector<std::vector<cv::Point2f>> corners, rejected;
        std::vector<int> ids;

        // Обнаружение ArUco маркеров
        cv::aruco::detectMarkers(img_maix, dictionary, corners, ids, detectorParams, rejected);

        // Подготавливаем JSON-результат
        Json::Value detected_markers; // или nlohmann::json detected_markers;

        // Записываем ids
        for (int id : ids) {
            detected_markers["ids"].append(id);
        }

        // Записываем углы
        for (const auto& corner : corners) {
            Json::Value corner_array;
            for (const auto& point : corner) {
                Json::Value pt;
                pt.append(point.x);
                pt.append(point.y);
                corner_array.append(pt);
            }
            detected_markers["corners"].append(corner_array);
        }

        // Записываем rejected
        for (const auto& rej : rejected) {
            Json::Value rej_array;
            for (const auto& point : rej) {
                Json::Value pt;
                pt.append(point.x);
                pt.append(point.y);
                rej_array.append(pt);
            }
            detected_markers["rejected"].append(rej_array);
        }

        // Создаём выходной пакет
        Packet* output_packet = new Packet(detected_markers, get_uptime_nanoseconds());
        set_pub_data_packet("detected_markers", output_packet);

        // Очистка
        delete output_packet;
        delete packet;
    }

private:
    cv::aruco::Dictionary dictionary;
    cv::aruco::DetectorParameters detectorParams = cv::aruco::DetectorParameters();
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
    // Catch signal and process
    sys::register_default_signal_handle();

    // Use CATCH_EXCEPTION_RUN_RETURN to catch exception,
    // if we don't catch exception, when program throw exception, the objects will not be destructed.
    // So we catch exception here to let resources be released(call objects' destructor) before exit.
    CATCH_EXCEPTION_RUN_RETURN(_main, -1, argc, argv);
}