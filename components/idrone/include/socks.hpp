// base_prepare/libraries/socks/socks.hpp
#pragma once

#include <zmq.hpp>
#include <msgpack.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>
#include <stdexcept>
#include <atomic>
#include <thread>
#include <mutex>
#include <iostream>
#include <cstdint>
#include <memory>
#include "maix_basic.hpp"
#include "maix_image.hpp"
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using json = nlohmann::json;

// === Big-endian сериализация (Python -> C++) ===
inline uint32_t htonll_be(uint32_t value) {
    return ((value >> 24) & 0x000000FF) |
           ((value >> 8)  & 0x0000FF00) |
           ((value << 8)  & 0x00FF0000) |
           ((value << 24) & 0xFF000000);
}

// === Big-endian десериализация (C++ <- Python) ===
inline uint32_t ntohll_be(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8)  |
           static_cast<uint32_t>(bytes[3]);
}

inline std::string get_local_ip_socks() {
    // Наивный подход к поиску IP - берёт первый из доступных. Работает только при отсутствии других интерфейсов
    std::string ip = "127.0.0.1";
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) return ip;

    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
            void* tmpAddrPtr = &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;
            char addressBuffer[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, tmpAddrPtr, addressBuffer, INET_ADDRSTRLEN);
            std::string ip_str = addressBuffer;
            if (ip_str != "127.0.0.1") {
                ip = ip_str;
                break;
            }
        }
    }
    freeifaddrs(ifaddr);
    return ip;
}

inline int64_t get_uptime_nanoseconds() {
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

inline cv::Mat maix_image_to_cv_mat(const maix::image::Image &img)
{
    if (img.format() == maix::image::Format::RGB888)
    {
        // Создаем cv::Mat из данных изображения
        return cv::Mat(img.height(), img.width(), CV_8UC3, (void *)img.data());
    }
    else if (img.format() == maix::image::Format::GRAYSCALE)
    {
        return cv::Mat(img.height(), img.width(), CV_8UC1, (void *)img.data());
    }
    else if (img.format() == maix::image::Format::RGB565)
    {
        // Нужно сначала преобразовать RGB565 -> RGB888
        maix::image::Image rgb888_img = img.to_format(maix::image::Format::RGB888);
        return cv::Mat(rgb888_img.height(), rgb888_img.width(), CV_8UC3, (void *)rgb888_img.data());
    }
    else
    {
        throw std::runtime_error("Unsupported image format");
    }
}

class FrameBuffer {
public:
    uint8_t* data_ptr;
    size_t size;

    FrameBuffer() {}

    FrameBuffer(uint8_t* data_ptr_, size_t size_, bool copy_) {
        if (!data_ptr_) {
            throw std::runtime_error("FrameBuffer object is empty, data_ptr is null");
        }

        data_ptr = (uint8_t*) malloc(size_);
        std::memmove(data_ptr, data_ptr_, size_);
        size = size_;

        // if (copy_) {
        // }
        // else {
        //     data_ptr = data_ptr_;
        //     size = size_;
        // }
    }

    ~FrameBuffer() {
        // std::cout << "Trying to destroy FrameBuffer. size: " << size << std::endl;
        if (data_ptr) {
            // Убираем const для корректного вызова free()
            // std::cout << "Before malloc " << size << std::endl;
            free(data_ptr);

            // uint8_t* test_big_data_ptr = (uint8_t*) malloc(size);
            // std::cout << "Before memmove " << std::endl;
            // std::memmove(test_big_data_ptr, data_ptr, size);
            // std::cout << "Before free " << std::endl;
            // free(test_big_data_ptr);

            // std::cout << "Right after free" << std::endl;
            // data_ptr = nullptr;
            // free(const_cast<uint8_t*>(data_ptr));
            // delete[] data_ptr;
        }
        size = 0;
    }

    uint8_t* get_data_ptr() const {
        if (!data_ptr) {
            throw std::runtime_error("FrameBuffer object is empty, data_ptr is null, can't get it");
        }
        uint8_t* data_ptr_new = (uint8_t*) malloc(size);
        std::memmove(data_ptr_new, data_ptr, size);

        return data_ptr_new;
    }

    FrameBuffer* copy() const {
        // FrameBuffer fb;
        // fb.size = size;
        // uint8_t* copy_data_ptr = (uint8_t*) malloc(size);
        // std::memcpy(copy_data_ptr, data_ptr, size);
        // fb.data_ptr = copy_data_ptr;
        if (!data_ptr) {
            throw std::runtime_error("FrameBuffer object already deleted, data_ptr is null");
        }
        return new FrameBuffer(data_ptr, size, true);
    }
};

class Packet {
public:
    json data;          // Все данные (включая изображения) хранятся здесь
    uint64_t emit_time_ns;
    FrameBuffer* frame;
    
    Packet* copy() const {
        if (frame) {
            return new Packet(data, emit_time_ns, frame->data_ptr, frame->size, true);
        }
        else {
            return new Packet(data, emit_time_ns);
        }
    }

    // Packet ref() {
    //     Packet packet;
    //     packet.data = data;
    //     packet.emit_time_ns = emit_time_ns;
    //     packet.frame = frame;
    //     return packet;
    // }
    Packet();

    Packet(json data_, uint64_t emit_time_ns_, uint8_t* data_ptr_, size_t size_, bool copy_) {
        data = data_;
        emit_time_ns = emit_time_ns_;
        if (data_ptr_) {
            frame = new FrameBuffer(data_ptr_, size_, copy_);
        }
    }

    Packet(json data_, uint64_t emit_time_ns_) {
        data = data_;
        emit_time_ns = emit_time_ns_;
    }

    static uint64_t get_emit_time_ns(const char* data, size_t size) {
        json j_packet = json::from_msgpack(
            reinterpret_cast<const uint8_t*>(data),
            reinterpret_cast<const uint8_t*>(data) + size
        );
        
        if (!j_packet.contains("emit_time_ns")) {
            throw std::runtime_error("Missing required fields in deserialized JSON");
        }

        return j_packet.at("emit_time_ns").get<uint64_t>();
    }

    static uint64_t get_emit_time_ns_image(const char* data, size_t size) {
        if (size < sizeof(uint32_t)) {
            throw std::runtime_error("Data too small for image format");
        }

        // Извлекаем длину метаданных (big-endian)
        uint32_t metadata_size = ntohll_be(reinterpret_cast<const uint8_t*>(data));
        if (size < sizeof(uint32_t) + metadata_size) {
            throw std::runtime_error("Incomplete metadata in image data");
        }

        // Парсим метаданные
        const uint8_t* metadata_start = reinterpret_cast<const uint8_t*>(data) + sizeof(uint32_t);
        json metadata = json::from_msgpack(metadata_start, metadata_start + metadata_size);

        return metadata["emit_time_ns"].get<uint64_t>();
    }

    // Сериализация обычного пакета
    static std::string serialize(const Packet* packet) {
        json j_packet = {
            {"data", packet->data},
            {"emit_time_ns", packet->emit_time_ns}
        };
        std::vector<uint8_t> msgpack_data = json::to_msgpack(j_packet);
        return std::string(msgpack_data.begin(), msgpack_data.end());
    }

    // Сериализация изображения (метаданные + raw данные)
    static std::string serialize_image(const Packet* packet) {
        // Проверяем структуру данных
        if (!packet->data.contains("shape") || !packet->data.contains("dtype")) {
            throw std::runtime_error("Invalid image data structure");
        }

        // Сериализуем метаданные (shape, dtype, emit_time_ns)
        json metadata = {
            {"shape", packet->data["shape"]},
            {"dtype", packet->data["dtype"]},
            {"emit_time_ns", packet->emit_time_ns}
        };
        std::vector<uint8_t> metadata_bytes = json::to_msgpack(metadata);
        uint32_t metadata_size = htonll_be(static_cast<uint32_t>(metadata_bytes.size()));

        // Получаем raw данные изображения (без копирования)
        char* frame_ptr = reinterpret_cast<char*>( packet->frame->data_ptr );
        size_t frame_size = packet->frame->size;

        // Формируем итоговый буфер:
        // [4 байта длина метаданных][метаданные][raw данные]
        std::string result;
        result.reserve(sizeof(uint32_t) + metadata_bytes.size() + frame_size);

        // Добавляем длину метаданных
        result.append(reinterpret_cast<char*>(&metadata_size), sizeof(uint32_t));
        // Добавляем метаданные
        result.append(reinterpret_cast<char*>(metadata_bytes.data()), metadata_bytes.size());
        // Добавляем raw данные изображения
        result.append(frame_ptr, frame_size);

        // std::cout << "Finish of serializing image" << std::endl;

        return result;
    }

    // Out
    static Packet* maix_image_to_packet(maix::image::Image* img, uint64_t emit_time_ns) {
        maix::image::Image* img_bgr888 = nullptr;
        if (img->format() != maix::image::FMT_BGR888) {
            img_bgr888 = img->to_format(maix::image::FMT_BGR888);
        } else {
            img_bgr888 = img->copy();
        }
        maix::Bytes* data_out = img_bgr888->to_bytes(true);
        delete img_bgr888;
        uint8_t* data_out_raw = data_out->begin();
        json metadata = {
            {"shape", std::vector<int>{img_bgr888->height(), img_bgr888->width(), 3}},
            {"dtype", "uint8"},
            {"emit_time_ns", emit_time_ns}
        };
        Packet* camera_img_packet_out = new Packet((json) metadata, (uint64_t) metadata["emit_time_ns"].get<uint64_t>(), 
                                                   (uint8_t*) data_out_raw, (size_t) data_out->size(), (bool) true);
        delete data_out;

        return camera_img_packet_out;
    }

    // Десериализация изображения (метаданные + raw данные)
    // Десериализация изображения: метаданные + raw данные
    static Packet* deserialize_image(const char* data, size_t size) {
        if (size < sizeof(uint32_t)) {
            throw std::runtime_error("Data too small for image format");
        }

        // Извлекаем длину метаданных (big-endian)
        uint32_t metadata_size = ntohll_be(reinterpret_cast<const uint8_t*>(data));
        if (size < sizeof(uint32_t) + metadata_size) {
            throw std::runtime_error("Incomplete metadata in image data");
        }

        // Парсим метаданные
        const uint8_t* metadata_start = reinterpret_cast<const uint8_t*>(data) + sizeof(uint32_t);
        json metadata = json::from_msgpack(metadata_start, metadata_start + metadata_size);

        // Создаём пакет
        // Packet packet;
        // packet.emit_time_ns = metadata["emit_time_ns"].get<uint64_t>();

        // Восстанавливаем метаданные изображения
        // packet.data["shape"] = metadata["shape"];
        // packet.data["dtype"] = metadata["dtype"];

        // Извлекаем raw данные изображения
        const uint8_t* frame_start = metadata_start + metadata_size;
        size_t frame_size = size - (sizeof(uint32_t) + metadata_size);

        // FrameBuffer framebuffer_new;
        // framebuffer_new.size = frame_size;
        
        // Сохраняем raw данные в вектор (минимум копирований)
        // packet.data["frame"] = std::vector<uint8_t>(frame_start, frame_start + frame_size);
        // uint8_t* copied_frame_start = (uint8_t*) malloc(frame_size);
        // std::memcpy(copied_frame_start, frame_start, frame_size);
        // framebuffer_new.data_ptr = copied_frame_start;

        // packet.frame = framebuffer_new;

        // std::cout << "Finish of deserializing image" << std::endl;

        Packet* res_packet = new Packet((json) metadata, (uint64_t) metadata["emit_time_ns"].get<uint64_t>(), (uint8_t*) frame_start, (size_t) frame_size, (bool) true);;

        // std::cout << "Returning result" << std::endl;

        return res_packet;
    }

    static maix::image::Image* packet_to_maix_image(const Packet* packet) {

        maix::Bytes* data_in = new maix::Bytes(packet->frame->data_ptr, packet->frame->size);
        maix::image::Image *img_transfered = maix::image::from_bytes((int) packet->data["shape"][1], (int) packet->data["shape"][0], maix::image::FMT_BGR888, data_in);

        delete data_in;

        return img_transfered;
    }

    static cv::Mat packet_to_cv_mat(const Packet* packet)
    {
        // Получаем размеры изображения
        int width  = (int) packet->data["shape"][1];
        int height = (int) packet->data["shape"][0];

        // Предполагаем, что формат — BGR888 (3 байта на пиксель)
        cv::Mat mat(height, width, CV_8UC3, (void*)packet->frame->data_ptr);

        // Возвращаем копию, чтобы владеть данными (если data_ptr может быть переиспользован/освобождён)
        cv::Mat result;
        mat.copyTo(result);

        return result;
    }

    static Packet* deserialize(const char* data, size_t size) {
        try {
            json j_packet = json::from_msgpack(
                reinterpret_cast<const uint8_t*>(data),
                reinterpret_cast<const uint8_t*>(data) + size
            );
            
            if (!j_packet.contains("data") || !j_packet.contains("emit_time_ns")) {
                throw std::runtime_error("Missing required fields in deserialized JSON");
            }

            return new Packet(j_packet.at("data"), j_packet.at("emit_time_ns").get<uint64_t>());
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] Deserialization failed: " << e.what() << std::endl;
            throw;
        }
    }

    static Packet* deserialize(const std::string& data) {
        return deserialize(data.data(), data.size());
    }

    ~Packet() {
        // std::cout << "Start erasing" << std::endl;

        if (frame) {
            delete frame;
        }

        // std::cout << "In process of erasing" << std::endl;
        // free(frame.data_ptr);
        // delete[] frame.data_ptr;
        // free(const_cast<uint8_t*>(frame.data_ptr));
        // data.erase();
        // std::cout << "Erased" << std::endl;
    }
    // ~Packet() = default;
};

class ConfigurableSocketModule {
public:
    using CommandHandler = std::function<void(const json&)>;

    explicit ConfigurableSocketModule(const std::string& controller_ip,
                                      const std::string& module_name);

    virtual ~ConfigurableSocketModule();

    void start();
    void stop();

    // Геттеры
    const std::unordered_map<std::string, std::unique_ptr<Packet>>& get_sub_data_packets() const {
        return sub_data_packets_;
    }

    std::unordered_map<std::string, std::unique_ptr<Packet>> get_sub_data_packets_copy() const {
        std::unordered_map<std::string, std::unique_ptr<Packet>> copy_map;
        for (const auto& item : sub_data_packets_) {
            if (item.second)
                copy_map[item.first] = std::unique_ptr<Packet>(item.second->copy());
        }
        return copy_map;
    }

    Packet* get_sub_data_packet(const std::string& key) {
        return sub_data_packets_[key].get()->copy();
    }

    bool check_sub_data_packet(const std::string& key) {
        return sub_data_packets_.count("frame");
    }

    const std::unordered_map<std::string, std::unique_ptr<Packet>>& get_pub_data_packets() const {
        return pub_data_packets_;
    }

    // Сеттеры
    void set_pub_data_packet(const std::string& key, Packet* packet) {
        // std::cout << "Before setting new pub packet 1" << std::endl;
        // pub_data_packets_.erase(key);
        // std::cout << "Before setting new pub packet 2" << std::endl;
        pub_data_packets_[key] = std::unique_ptr<Packet>(packet->copy());
        // std::cout << "Setted new pub packet" << std::endl;
    }

    void send_status_to_controller(const std::string& status);
    void safe_send_error(const std::string& source, const std::string& message);

    void clear_all_pub_out() {
        // std::cout << "Before clearing pub packets" << std::endl;
        pub_data_packets_.clear(); // ~Packet вызывается автоматически
    }

    // void ConfigurableSocketModule::clear_all_pub_out() {
    //     // for (const auto& item : pub_data_packets_) {
    //     //     // if (item.second->frame->data_ptr) {
    //     //     //     free(item.second->frame->data_ptr); // Освобождаем память
    //     //     // }
    //     //     delete item.second->frame;
    //     // }
    //     std::cout << "Before clearing" << std::endl;
    //     pub_data_packets_.clear(); 
    // }

    void clear_all_sub_in() {
        // std::cout << "Clearing packets sub in" << std::endl;
        // for (const auto& item : sub_data_packets_) {
        //     // if (item.second->frame->data_ptr) {
        //     //     free(item.second->frame->data_ptr); // Освобождаем память
        //     // }
        //     // delete item.second;
        //     std::cout << "Trying to call destructor for " << item.first << std::endl;
        //     sub_data_packets_.erase(item.first);
        // }
        sub_data_packets_.clear(); 
    }

protected:
    // Переопределяемые методы
    virtual void input_run() {}
    virtual void output_run() {}
    virtual void processor_run() = 0;

private:
    void command_thread();
    void input_polling();
    void output_sending();
    void processor_loop();

    void configure_sockets(const json& socket_config);

    std::string name_;
    zmq::context_t context_;
    zmq::socket_t status_socket_;
    zmq::socket_t command_socket_;

    std::unordered_map<std::string, zmq::socket_t> sub_sockets_;
    std::unordered_map<std::string, zmq::socket_t> pub_sockets_;

    std::unordered_map<std::string, std::unique_ptr<Packet>> sub_data_packets_;
    std::unordered_map<std::string, std::unique_ptr<Packet>> pub_data_packets_;

    std::unordered_map<std::string, uint64_t> last_emit_times_;

    std::atomic<bool> running_{true};
    std::atomic<bool> paused_{true};

    std::atomic<bool> has_new_data_{false};

    std::string module_type_ = "unknown"; // Поле для хранения типа модуля
    json base_params_;                    // Базовые параметры из config.yaml
    json params_;                         // Текущие параметры (могут обновляться)
    std::mutex params_mutex_;             // Мьютекс для синхронизации доступа к params_
    std::mutex data_in_mutex;
    std::mutex data_out_mutex;


    json config_;
    json sockets_config_;
};