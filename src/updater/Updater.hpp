#pragma once

#include "http/HttpHelper.hpp"
#include "core/stealth/Obfuscate.hpp"

struct Status {
    bool unsafe;
    std::string notice;

    int version_current;
    int version_minimum;
};

class Updater {
public:
    ~Updater() = default;
    Updater(const Updater&) = delete;
    Updater(Updater&&) = delete;
    Updater& operator=(const Updater&) = delete;
    Updater& operator=(Updater&&) = delete;

    static bool Init();
    static bool Update(); // Not Implemented Yet
    static bool Process();

    static Status GetStatus();
private:
    Updater() {};

    static Updater& GetInstance()
    {
        static Updater i{};
        return i;
    }

    bool InitImpl();
    bool ProcessImpl();
private:
    Status status;
    bool isSetup = false;
    int current_version = 122;
    std::string status_url = OBF("https://github.com/Dekrypte0/cs2-external/raw/refs/heads/main/.github/status.json");
};