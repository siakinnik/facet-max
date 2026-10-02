// The running app: a private D-Bus session bus (the bundled dbus-daemon) with
// our secret and notification services on it, and MAX itself on the Wayland
// display the compositor module shares with this module.
#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

#include "dbus.h"

namespace maxmod {

struct SessionPaths {
    std::string runtime;  // bundled libraries, dbus-daemon, Qt plugin, fonts, shim
    std::string app;      // the unpacked MAX (…/app/current)
    std::string home;     // HOME of MAX
    std::string wayland;  // the compositor's socket
    std::string bus;      // where our session bus listens
    std::string locale;   // Facet's UI language ("ru", "en")
};

class Session {
public:
    ~Session() { stop(); }
    // Starts the bus (if needed) and MAX. English error on failure.
    bool start(const SessionPaths& paths, std::string& error);
    void stop();
    bool running() const { return max_pid_ > 0; }
    bool bus_up() const { return bus_.connected(); }
    dbus::Connection& bus() { return bus_; }
    // Reaps exited processes; returns true if MAX exited since the last call.
    bool reap(int& status);

private:
    bool start_bus(const SessionPaths& paths, std::string& error);
    pid_t dbus_pid_ = -1, max_pid_ = -1;
    dbus::Connection bus_;
};

// The environment MAX runs with (exposed for tests).
std::vector<std::string> max_environment(const SessionPaths& paths);

}  // namespace maxmod
