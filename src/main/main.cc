// SPDX-License-Identifier: MIT
#include "core/client.h"
#include "core/server.h"
#include "utility/random.h"
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <libconfig.h++>
#include <mutex>
#include <signal.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>

#if defined(_WIN32) || defined(_WIN64)
#include <winsock.h>
#endif

#include <argp.h>

namespace {

struct arguments {
    std::string mode;
    std::string websocket;
    std::string tun;
    std::string dhcp;
    std::string password;
    std::string name;
    std::string stun;
};

const int OPT_NO_TIMESTAMP = 1;
const int OPT_LOG_LEVEL_DEBUG = 2;

struct argp_option options[] = {
    {"mode", 'm', "TEXT", 0, "The process works in client or server mode"},
    {"websocket", 'w', "URI", 0, "Server listening address"},
    {"tun", 't', "CIDR", 0, "Static configured IP address"},
    {"dhcp", 'd', "CIDR", 0, "Automatically assigned address range"},
    {"password", 'p', "TEXT", 0, "Authorization password consistent with the server"},
    {"name", 'n', "TEXT", 0, "Network interface name"},
    {"stun", 's', "URI", 0, "STUN service address"},
    {"config", 'c', "PATH", 0, "Configuration file path"},
    {"no-timestamp", OPT_NO_TIMESTAMP, 0, 0, "Log does not show time"},
    {"debug", OPT_LOG_LEVEL_DEBUG, 0, 0, "Show debug level logs"},
    {},
};

int disableLogTimestamp() {
    spdlog::set_pattern("[%^%l%$] %v");
    return 0;
}

int setLogLevelDebug() {
    spdlog::set_level(spdlog::level::debug);
    spdlog::debug("set log level: debug");
    return 0;
}

bool needShowUsage(struct arguments *arguments, struct argp_state *state) {
    if (state->arg_num > 0)
        return true;

    if (arguments->mode != "client" && arguments->mode != "server")
        return true;

    if (arguments->websocket.empty())
        return true;

    return false;
}

void parseConfigFile(struct arguments *arguments, std::string config) {
    try {
        libconfig::Config cfg;
        cfg.readFile(config.c_str());
        cfg.lookupValue("mode", arguments->mode);
        cfg.lookupValue("websocket", arguments->websocket);
        cfg.lookupValue("tun", arguments->tun);
        cfg.lookupValue("dhcp", arguments->dhcp);
        cfg.lookupValue("stun", arguments->stun);
        cfg.lookupValue("password", arguments->password);
        cfg.lookupValue("name", arguments->name);
    } catch (const libconfig::FileIOException &fioex) {
        spdlog::critical("i/o error while reading configuration file");
        exit(1);
    } catch (const libconfig::ParseException &pex) {
        spdlog::critical("parse error at {} : {} - {}", pex.getFile(), pex.getLine(), pex.getError());
        exit(1);
    }
}

int parseOption(int key, char *arg, struct argp_state *state) {
    struct arguments *arguments = (struct arguments *)state->input;

    switch (key) {
    case 'm':
        arguments->mode = arg;
        break;
    case 'w':
        arguments->websocket = arg;
        break;
    case 't':
        arguments->tun = arg;
        break;
    case 'd':
        arguments->dhcp = arg;
        break;
    case 'p':
        arguments->password = arg;
        break;
    case 'n':
        arguments->name = arg;
        break;
    case 's':
        arguments->stun = arg;
        break;
    case 'c':
        parseConfigFile(arguments, arg);
        break;
    case OPT_NO_TIMESTAMP:
        disableLogTimestamp();
        break;
    case OPT_LOG_LEVEL_DEBUG:
        setLogLevelDebug();
        break;
    case ARGP_KEY_END:
        if (needShowUsage(arguments, state))
            argp_usage(state);
        break;
    }
    return 0;
}

struct argp config = {
    .options = options,
    .parser = parseOption,
};

volatile int exitCode = 0;
volatile bool running = true;
std::mutex mutex;
std::condition_variable condition;

void signalHandler(int signal) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        running = false;
    }
    condition.notify_one();
}

#if defined(_WIN32) || defined(_WIN64)

bool netStartup() {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

bool netCleanup() {
    return WSACleanup() == 0;
}

std::string storageDirectory = "C:/ProgramData/Candy/";

#else

bool netStartup() {
    return true;
}

bool netCleanup() {
    return true;
}

std::string storageDirectory = "/var/lib/candy/";

#endif

int saveLatestAddress(const std::string &name, const std::string &cidr) {
    std::string cache = storageDirectory + "address/";
    cache += name.empty() ? "__noname__" : name;
    std::filesystem::create_directories(std::filesystem::path(cache).parent_path());
    std::ofstream ofs(cache);
    if (ofs.is_open()) {
        ofs << cidr;
        ofs.close();
    }
    return 0;
}

std::string getLastestAddress(const std::string &name) {
    std::string cache = storageDirectory + "address/";
    cache += name.empty() ? "__noname__" : name;
    std::ifstream ifs(cache);
    if (ifs.is_open()) {
        std::stringstream ss;
        ss << ifs.rdbuf();
        ifs.close();
        return ss.str();
    }
    return "";
}

std::string getVirtualMac(const std::string &name) {
    std::string cache = storageDirectory + "vmac/";
    cache += name.empty() ? "__noname__" : name;
    std::filesystem::create_directories(std::filesystem::path(cache).parent_path());

    char buffer[16];
    std::stringstream ss;

    std::ifstream ifs(cache);
    if (ifs.is_open()) {
        ifs.read(buffer, sizeof(buffer));
        if (ifs) {
            for (int i = 0; i < (int)sizeof(buffer); i++) {
                ss << std::hex << buffer[i];
            }
        }
        ifs.close();
    } else {
        ss << Candy::randomHexString(sizeof(buffer));
        std::ofstream ofs(cache);
        if (ofs.is_open()) {
            ofs << ss.str();
            ofs.close();
        }
    }
    return ss.str();
}

} // namespace

namespace Candy {
void shutdown() {
    exitCode = 1;
    signalHandler(SIGTERM);
}
} // namespace Candy

int main(int argc, char *argv[]) {
    netStartup();

    Candy::Server server;
    Candy::Client client;

    struct arguments arguments;
    argp_parse(&config, argc, argv, 0, 0, &arguments);

    if (arguments.mode == "server") {
        server.setPassword(arguments.password);
        server.setWebSocketServer(arguments.websocket);
        server.setDynamicAddressRange(arguments.dhcp);
        server.run();
    }

    if (arguments.mode == "client") {
        client.setupAddressUpdateCallback([&](const std::string &cidr) { saveLatestAddress(arguments.name, cidr); });
        client.setPassword(arguments.password);
        client.setWebSocketServer(arguments.websocket);
        client.setStun(arguments.stun);
        client.setLocalAddress(arguments.tun);
        client.setDynamicAddress(getLastestAddress(arguments.name));
        client.setVirtualMac(getVirtualMac(arguments.name));
        client.setName(arguments.name);
        client.run();
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return !running; });
    }

    server.shutdown();
    client.shutdown();

    if (exitCode == 0) {
        spdlog::info("service exit: normal");
    } else {
        spdlog::warn("service exit: internal exception");
    }

    netCleanup();

    return exitCode;
}