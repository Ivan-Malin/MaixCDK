#include "socks.hpp"
#include <zmq.hpp>
#include <msgpack.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream> // Для проверки config.yaml
#include <yaml-cpp/yaml.h>

using json = nlohmann::json;

inline nlohmann::json parse_scalar(const YAML::Node &node) {
  int i;
  double d;
  bool b;
  std::string s;

  if (YAML::convert<int>::decode(node, i))
    return i;
  if (YAML::convert<double>::decode(node, d))
    return d;
  if (YAML::convert<bool>::decode(node, b))
    return b;
  if (YAML::convert<std::string>::decode(node, s))
    return s;

  return nullptr;
}

inline nlohmann::json yaml2json(const YAML::Node &root) {
  nlohmann::json j{};

  switch (root.Type()) {
  case YAML::NodeType::Null:
    break;
  case YAML::NodeType::Scalar:
    return parse_scalar(root);
  case YAML::NodeType::Sequence:
    for (auto &&node : root)
      j.emplace_back(yaml2json(node));
    break;
  case YAML::NodeType::Map:
    for (auto &&it : root)
      j[it.first.as<std::string>()] = yaml2json(it.second);
    break;
  default:
    break;
  }
  return j;
}

ConfigurableSocketModule::ConfigurableSocketModule(const std::string& controller_ip,
                                                   const std::string& module_name) :
    name_(module_name),
    context_(1),
    status_socket_(context_, ZMQ_PUB),
    command_socket_(context_, ZMQ_SUB) {

    // === 1. Проверка наличия config.yaml ===
    std::ifstream config_file("config.yaml");
    if (!config_file.is_open()) {
        std::cerr << "[ERROR] config.yaml not found in module's directory" << std::endl;
        std::exit(EXIT_FAILURE);
    }

    try {
        json config = yaml2json(YAML::Load(config_file));
        sockets_config_ = config.value("sockets", json::object());
        module_type_ = config.value("type", "unknown"); // === 2. Чтение типа модуля ===
        base_params_ = config.value("params", json::object());
        params_ = base_params_; // Инициализация текущих параметров базовыми
    } catch (const json::parse_error& e) {
        std::cerr << "[ERROR] Error parsing config.yaml: " << e.what() << std::endl;
        std::exit(EXIT_FAILURE);
    }
    std::cout << "C++ GOT MODULE TYPE: " << module_type_ << std::endl;

    std::cout << "C++ AT LEAST CONSTRUCTING, HELL YEAH" << std::endl;

    status_socket_.connect("tcp://" + controller_ip + ":9001");
    std::cout << "C++ AT LEAST CONSTRUCTING 1, HELL YEAH" << std::endl;
    command_socket_.connect("tcp://" + controller_ip + ":9000");
    std::cout << "C++ AT LEAST CONSTRUCTING 2, HELL YEAH" << std::endl;
    command_socket_.set(zmq::sockopt::subscribe, "");
    std::cout << "C++ AT LEAST CONSTRUCTING 2, HELL YEAH" << std::endl;

    std::cout << "C++ AT LEAST CONNECTING, HELL YEAH" << std::endl;

    send_status_to_controller("ready");
}

ConfigurableSocketModule::~ConfigurableSocketModule() {
    running_ = false;
    for (auto& pair : sub_sockets_) { pair.second.close(); }
    for (auto& pair : pub_sockets_) { pair.second.close(); }
    context_.close();
}

void ConfigurableSocketModule::start() {
    std::cout << "C++ AT LEAST STARTING, HELL YEAH" << std::endl;
    running_ = true;
    std::thread(&ConfigurableSocketModule::command_thread, this).detach();
    std::thread(&ConfigurableSocketModule::input_polling, this).detach();
    std::thread(&ConfigurableSocketModule::output_sending, this).detach();
    std::thread(&ConfigurableSocketModule::processor_loop, this).detach();
}

void ConfigurableSocketModule::stop() {
    running_ = false;
}

void ConfigurableSocketModule::configure_sockets(const json& socket_config) {
    // Очищаем старые входящие сокеты
    for (const auto& pair : sub_sockets_) {
        if (sub_sockets_.count(pair.first)) sub_sockets_.erase(pair.first);
    }

    // Очищаем старые исходящие сокеты
    for (const auto& pair : pub_sockets_) {
        if (pub_sockets_.count(pair.first)) pub_sockets_.erase(pair.first);
    }

    // Подключаем новые sock_in
    if (socket_config.contains("sock_in") && socket_config["sock_in"].is_object()) {
        const auto& sock_in = socket_config["sock_in"];
        for (auto it = sock_in.begin(); it != sock_in.end(); ++it) {
            const std::string& name = it.key();
            const std::string& remote = it.value().get<std::string>();

            zmq::socket_t sock(context_, ZMQ_SUB);
            int trueValue = 1;
            zmq_setsockopt(sock, ZMQ_SUBSCRIBE, "", 0);
            zmq_setsockopt(sock, ZMQ_CONFLATE, &trueValue, sizeof(int));
            sock.connect(remote.c_str());
            sock.set(zmq::sockopt::subscribe, "");
            sub_sockets_[name] = std::move(sock);
        }
    }

    // Привязываем новые sock_out
    if (socket_config.contains("sock_out") && socket_config["sock_out"].is_object()) {
        const auto& sock_out = socket_config["sock_out"];
        for (auto it = sock_out.begin(); it != sock_out.end(); ++it) {
            const std::string& name = it.key();
            const std::string& remote = it.value().get<std::string>();

            zmq::socket_t sock(context_, ZMQ_PUB);
            sock.bind(remote.c_str());
            pub_sockets_[name] = std::move(sock);
        }
    }

    std::cout << "C++ I DID CONFIGURED SOCKETS" << std::endl;
    paused_ = false;
}

void ConfigurableSocketModule::send_status_to_controller(const std::string& status) {
    json msg = {
        {"module", name_},
        {"status", status},
        {"ip", get_local_ip()}
        {"timestamp", std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count()}
    };

    zmq::message_t zmsg(msg.dump().size());
    memcpy(zmsg.data(), msg.dump().c_str(), msg.dump().size());
    status_socket_.send(zmsg, zmq::send_flags::none);
    std::cout << "C++ AT SENDING STATUS, HELL YEAH" << std::endl;
}

void ConfigurableSocketModule::safe_send_error(const std::string& source, const std::string& message) {
    json err = {
        {"type", "error"},
        {"module", name_},
        {"timestamp", std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count()},
        {"message", message},
        {"source", source}
    };
    zmq::message_t zmsg(err.dump().size());
    memcpy(zmsg.data(), err.dump().c_str(), err.dump().size());
    status_socket_.send(zmsg, zmq::send_flags::none);
}

void ConfigurableSocketModule::command_thread() {
    while (running_) {
        zmq::message_t msg;
        if (command_socket_.recv(msg)) {
            try {
                auto j = json::parse(msg.to_string());
                if (j.contains("mod_name") && j["mod_name"] != name_) continue;

                if (j["command"] == "configure") {
                    configure_sockets(j["sockets"]);

                    // === Обновление параметров ===
                    if (j.contains("params")) {
                        // std::lock_guard<std::mutex> lock(params_mutex_);
                        for (auto& [key, value] : j["params"].items()) {
                            params_[key] = value; // Перезапись параметров
                        }
                    }
                } else if (j["command"] == "resume") {
                    std::cout << "C++ GOT COMMAND TO RESUME";
                    paused_ = false;
                } else if (j["command"] == "pause") {
                    std::cout << "C++ GOT COMMAND TO PAUSE";
                    paused_ = true;
                } else if (j["command"] == "kill") {
                    stop();
                }
            } catch (...) {
                safe_send_error("command", "Parse error");
            }
        }
    }
}

// === 4. Условный запуск processor_run ===
void ConfigurableSocketModule::input_polling() {
    while (running_) {
        if (!paused_) {
            std::lock_guard<std::mutex> lock(data_in_mutex);
            for (auto& [name, sock] : sub_sockets_) {
                zmq::message_t msg;
                // std::cout << "Start of recieving msg" << std::endl;
                if (sock.recv(msg, zmq::recv_flags::dontwait)) {
                    // std::cout << "C++ TRYING TO RECIEVE AND SERIALIZE 0" << std::endl;
                    try {
                        // std::cout << "C++ GETTING DATA TYPE" << std::endl;
                        // std::cout << "C++ TRYING TO RECIEVE AND SERIALIZE 1" << std::endl;
                        std::string data_type = sockets_config_["sock_in"][name];
                        
                        // std::cout << "C++ TRYING TO RECIEVE AND SERIALIZE 2" << std::endl;
                        // std::cout << "Start of deserializing packet" << std::endl;
                        // std::cout << "Start of checking packet" << std::endl;
                        // std::cout << "C++ GOT NEW DATA HELL YEAH" << std::endl;
                        // std::cout << "C++ GETTING EMIT TIME" << std::endl;
                        uint64_t emit_time_ns;
                        if (data_type == "image") {
                            emit_time_ns = Packet::get_emit_time_ns_image(static_cast<const char*>(msg.data()), msg.size());
                        } else {
                            emit_time_ns = Packet::get_emit_time_ns(static_cast<const char*>(msg.data()), msg.size());
                        }

                        // std::cout << "C++ PERFORMING CHECK" << std::endl;

                        if (sub_data_packets_.count(name) == 0 ||
                            sub_data_packets_[name]->emit_time_ns != emit_time_ns) {

                            // std::cout << "C++ ADDING PACKET" << std::endl;
                        
                            // free(sub_data_packets_[name].frame.data_ptr);
                            // auto old_packet = sub_data_packets_[name];

                            // Packet* packet;
                            sub_data_packets_.erase(name);
                            if (data_type == "image") {
                                sub_data_packets_[name] = std::unique_ptr<Packet>(Packet::deserialize_image(static_cast<const char*>(msg.data()), msg.size()));
                                // std::cout << "Deserialized packet on input polling" << std::endl;
                            } else {
                                sub_data_packets_[name] = std::unique_ptr<Packet>(Packet::deserialize(static_cast<const char*>(msg.data()), msg.size()));
                            }
                            
                            // ((Packet*) sub_data_packets_[name])->~Packet();
                            // std::cout << "Set sub packet" << std::endl;
                            // sub_data_packets_[name] = packet;
                            // if (old_packet->frame->data_ptr) {
                            //     free(old_packet->frame->data_ptr);
                            // }
                            has_new_data_ = true;
                        }
                        // std::cout << "End of checking packet" << std::endl;
                    } catch (std::exception const & ex) {
                        std::string full_error = std::string("Deserialization error: ") + ex.what();
                        safe_send_error("input", full_error.c_str());
                    }
                }
                // std::cout << "End of recieving msg" << std::endl;
            }
            // std::cout << "End of iterating packets" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void ConfigurableSocketModule::output_sending() {
    while (running_) {
        if (!paused_) {
            // Прямой доступ к pub_data_packets_ без мьютекса
            std::lock_guard<std::mutex> lock(data_out_mutex);
            for (auto& [name, packet] : pub_data_packets_) {
                zmq::socket_t* sock = &pub_sockets_[name];
                if (sock) {
                    try {
                        std::string data_type = sockets_config_["sock_out"][name];

                        uint64_t current_emit_time = packet->emit_time_ns;

                        // Проверка дубликатов
                        if (last_emit_times_.count(name) != 0 && last_emit_times_[name] == current_emit_time) {
                            continue; // Пропускаем отправку
                        }
                        std::string serialized;

                        if (data_type == "image") {
                            serialized = Packet::serialize_image(packet.get());
                        } else {
                            serialized = Packet::serialize(packet.get());
                        }

                        // if (packet->frame->data_ptr) {
                        //     free(packet->frame->data_ptr);
                        // }

                        // Отправка пакета
                        sock->send(zmq::buffer(serialized));
                        last_emit_times_[name] = current_emit_time; // Обновление времени
                        // std::cout << "Before erasing after set" << std::endl;
                        // pub_data_packets_.erase(name);
                    } catch (std::exception const & ex) {
                        std::string full_error = std::string("Send error: ") + ex.what();
                        safe_send_error("output", full_error.c_str());
                    }
                }
            }
            // clear_all_pub_out();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void ConfigurableSocketModule::processor_loop() {
    while (running_) {
        if (!paused_) {
            try {
                // === Условие запуска ===
                if (module_type_ == "sensor" || (module_type_ != "sensor" && (has_new_data_))) {
                    std::lock_guard<std::mutex> lock_in(data_in_mutex);
                    std::lock_guard<std::mutex> lock_out(data_out_mutex);
                    processor_run();
                    // clear_all_sub_in();
                    has_new_data_ = false;
                }
            } catch (std::exception const & ex) {
                std::string full_error = std::string("Runtime error: ") + ex.what();
                safe_send_error("run", full_error.c_str());
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}