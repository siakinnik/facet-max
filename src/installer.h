// Downloads and installs MAX from its official repository on the device
// (the module does not redistribute MAX). Runs in a background thread; the
// plugin thread polls status().
//
// Layout under the data directory: app/<version>/ (the unpacked app),
// app/current -> <version>, download/ (the .deb while it is fetched).
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "package.h"

namespace maxmod {

constexpr const char* kRepository = "https://download.max.ru/linux/deb/";
constexpr const char* kIndex = "dists/stable/main/binary-amd64/Packages";

class Installer {
public:
    enum class Phase { Idle, Checking, Downloading, Verifying, Extracting, Done, Failed };
    struct Status {
        Phase phase = Phase::Idle;
        double progress = 0;  // 0..1 within the phase
        uint64_t total = 0;   // bytes to download
        std::string error;    // English, when Failed
        PackageInfo latest;   // after a check
        bool installed_now = false;  // Done after installing (not only checking)
    };

    explicit Installer(std::string data_dir);
    ~Installer();

    // Looks up the newest version; with `install`, also installs it unless it is current.
    void start(bool install);
    void cancel();
    bool busy() const;
    Status status() const;
    void acknowledge();  // Done/Failed -> Idle

    std::string installed_version() const;  // "" if none
    std::string app_dir() const;            // data/app/current
    // Removes the app (not the user's data).
    void remove_app();

private:
    void run(bool install);
    void set(Phase p, double progress = 0);

    std::string data_;
    mutable std::mutex mu_;
    Status status_;
    std::thread worker_;
    std::atomic<bool> cancel_{false}, running_{false};
};

// rm -rf
void remove_tree(const std::string& path);

}  // namespace maxmod
