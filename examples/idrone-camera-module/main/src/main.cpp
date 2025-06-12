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

using namespace maix;

class CameraModule : public ConfigurableSocketModule {
public:
    CameraModule(const std::string& controller_ip, const std::string& module_name, camera::Camera* _cam) :
        ConfigurableSocketModule(controller_ip, module_name),
        cam(_cam) {
        // Никакой дополнительной инициализации не требуется
        // cam_high_res = cam.add_channel(640, 480);
        cam_low_res = cam->add_channel(320, 240);
    }

protected:
    camera::Camera* cam;
    
    // camera::Camera *cam_high_res;
    camera::Camera *cam_low_res;

    void processor_run() override {
        // Проверяем, есть ли новое изображение
        maix::image::Image *img_low_res = nullptr;
        try {
            // img_high_res = cam_high_res->read();
            img_low_res  = cam_low_res->read();
            // img_low_res->resize(320, 240, image::Fit::FIT_FILL, image::ResizeMethod::NEAREST);
        } catch (std::exception &e) {
            time::sleep_ms(10);
            return;
        }

        // Out
        Packet* low_res_packet = Packet::maix_image_to_packet(img_low_res, get_uptime_nanoseconds());

        // Отправляем результат
        set_pub_data_packet("output_frame", low_res_packet);
        delete low_res_packet;
        delete img_low_res;
    }
};

int _main(int argc, char* argv[])
{
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <controller_ip> <module_name>" << std::endl;
        return 1;
    }
    
    std::string controller_ip = argv[1];
    std::string module_name = argv[2];

    int cam_w = -1;
    int cam_h = -1;
    image::Format cam_fmt = image::Format::FMT_YVU420SP;
    int cam_fps = -1;
    int cam_buffer_num = 3;

    camera::Camera cam = camera::Camera(cam_w, cam_h, cam_fmt, "", cam_fps, cam_buffer_num);
    auto audio_recorder = audio::Recorder();
    rtsp::Rtsp rtsp = rtsp::Rtsp();
    rtsp.bind_camera(&cam);
    rtsp.bind_audio_recorder(&audio_recorder);
    
    // Show debug info
    log::info("url:%s", rtsp.get_url().c_str());
    std::vector<std::string> url = rtsp.get_urls();
    for (size_t i = 0; i < url.size(); i ++) {
        log::info("url[%d]:%s", i, url[i].c_str());
    }
    err::check_raise(rtsp.start());
    std::cout << "Started" << std::endl;
    
    CameraModule cameraModule(controller_ip, module_name, &cam);
    cameraModule.start();

    while(!app::need_exit()) {
        time::sleep_ms(1000);
    }
    cameraModule.stop();
    rtsp.stop();

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