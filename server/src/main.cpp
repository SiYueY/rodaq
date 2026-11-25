#include "server_app.hpp"
#include "server_config.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>
#include <rodaq/logging.hpp>

namespace {

std::atomic<bool> g_shutdown_requested{false};

void handle_signal(int /*sig*/) noexcept {
    g_shutdown_requested.store(true, std::memory_order_release);
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path = "/etc/rodaq/deployment.json";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--version") {
            std::cout << "rodaq 0.1.0\n";
            return 0;
        }
    }

    std::ifstream in(config_path);
    if (!in.good()) {
        std::cerr << "rodaq: failed to open deployment config: " << config_path << "\n";
        return 2;
    }
    std::ostringstream ss;
    ss << in.rdbuf();

    try {
        const auto doc = nlohmann::json::parse(ss.str());
        if (!doc.is_object() || !doc.contains("server")) {
            std::cerr << "rodaq: deployment.json missing required 'server' object\n";
            return 2;
        }
        auto server_cfg = rodaq::server::parse_server_config(doc["server"].dump());
        if (!server_cfg.ok()) {
            std::cerr << "rodaq: invalid server config: " << server_cfg.status().message() << "\n";
            return 2;
        }
        std::signal(SIGTERM, handle_signal);
        std::signal(SIGINT, handle_signal);
        std::cout << "rodaq: validated server config for " << server_cfg.value().http.listen_address
                  << ":" << server_cfg.value().http.port << "\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "rodaq: fatal startup error: " << ex.what() << "\n";
        return 2;
    }
}
