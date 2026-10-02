#include "session.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

extern char** environ;

namespace maxmod {

namespace {

// fork + exec with a complete environment, in its own process group.
pid_t spawn(const std::vector<std::string>& argv, const std::vector<std::string>& env, const std::string& cwd) {
    std::vector<char*> a, e;
    for (const auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
    a.push_back(nullptr);
    for (const auto& s : env) e.push_back(const_cast<char*>(s.c_str()));
    e.push_back(nullptr);
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, 0);  // stdin/stdout are Facet's protocol pipes
            dup2(2, 1);     // output goes to the module log
        }
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        signal(SIGPIPE, SIG_DFL);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
        execve(a[0], a.data(), e.data());
        _exit(127);
    }
    return pid;
}

void terminate(pid_t& pid) {
    if (pid <= 0) return;
    ::kill(-pid, SIGTERM);
    for (int i = 0; i < 30; ++i) {  // up to 3 s, then for good
        if (waitpid(pid, nullptr, WNOHANG) == pid) {
            pid = -1;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::kill(-pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    pid = -1;
}

std::string env_of(const char* name, const char* def = "") {
    const char* v = std::getenv(name);
    return v ? v : def;
}

}  // namespace

std::vector<std::string> max_environment(const SessionPaths& p) {
    // Only C.UTF-8 is sure to exist on the host (MAX aborts on a missing
    // locale); Qt takes the UI language from LANGUAGE.
    return {
        "HOME=" + p.home,
        "PATH=/usr/local/bin:/usr/bin:/bin",
        "LANG=C.UTF-8",
        "LANGUAGE=" + p.locale,
        "TZ=" + env_of("TZ"),
        "TMPDIR=/tmp",
        "XDG_RUNTIME_DIR=/tmp",
        "XDG_SESSION_TYPE=wayland",
        "WAYLAND_DISPLAY=" + p.wayland,
        "DBUS_SESSION_BUS_ADDRESS=unix:path=" + p.bus,
        // MAX ships Qt without its Wayland platform plugin: ours (official
        // Qt build of the same version) comes with the module, and the shim
        // turns MAX's built-in "-platform xcb" into "wayland".
        "QT_QPA_PLATFORM=wayland",
        "QT_QPA_PLATFORM_PLUGIN_PATH=" + p.runtime + "/plugins/platforms",
        "QT_QPA_FONTDIR=" + p.runtime + "/fonts",
        "QT_WAYLAND_DISABLE_WINDOWDECORATION=1",
        "QT_QUICK_BACKEND=software",
        "FONTCONFIG_FILE=" + p.runtime + "/etc/fonts.conf",
        // No GPU in the compositor; Chromium's own sandbox cannot nest in the
        // module's container, which already isolates it.
        "QTWEBENGINE_DISABLE_SANDBOX=1",
        "QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu --no-sandbox",
        "LD_LIBRARY_PATH=" + p.app + "/lib64:" + p.app + "/lib:" + p.runtime + "/lib",
        "LD_PRELOAD=" + p.runtime + "/lib/facet-max-shim.so",
    };
}

bool Session::start_bus(const SessionPaths& p, std::string& error) {
    if (bus_.connected() && dbus_pid_ > 0) return true;
    terminate(dbus_pid_);
    bus_.close();
    ::unlink(p.bus.c_str());
    dbus_pid_ = spawn({p.runtime + "/bin/dbus-daemon", "--config-file=" + p.runtime + "/share/dbus-session.conf",
                       "--nofork", "--nopidfile", "--address=unix:path=" + p.bus},
                      {"LD_LIBRARY_PATH=" + p.runtime + "/lib", "PATH=/usr/bin:/bin", "HOME=" + p.home}, {});
    if (dbus_pid_ < 0) {
        error = "cannot start dbus-daemon";
        return false;
    }
    for (int i = 0; i < 50; ++i) {  // up to 5 s for the socket
        struct stat st;
        if (::stat(p.bus.c_str(), &st) == 0 && bus_.connect(p.bus, error)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!bus_.connected()) {
        if (error.empty()) error = "the session bus did not start";
        terminate(dbus_pid_);
        return false;
    }
    bus_.request_name("org.freedesktop.secrets");
    bus_.request_name("org.freedesktop.Notifications");
    return true;
}

bool Session::start(const SessionPaths& p, std::string& error) {
    if (max_pid_ > 0) return true;
    if (!start_bus(p, error)) return false;
    ::mkdir(p.home.c_str(), 0700);
    std::vector<std::string> env = max_environment(p);
    max_pid_ = spawn({p.app + "/bin/max"}, env, p.app + "/bin");
    if (max_pid_ < 0) {
        error = "cannot start MAX";
        return false;
    }
    return true;
}

void Session::stop() {
    terminate(max_pid_);
    bus_.close();
    terminate(dbus_pid_);
}

bool Session::reap(int& status) {
    // In its container the plugin is PID 1: orphans of the app (web engine
    // helpers) are its children too, so reap everything.
    bool exited = false;
    int st = 0;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
        if (pid == max_pid_) {
            ::kill(-max_pid_, SIGTERM);  // helpers go with it
            max_pid_ = -1;
            status = st;
            exited = true;
        } else if (pid == dbus_pid_) {
            dbus_pid_ = -1;
            bus_.close();
        }
    }
    return exited;
}

}  // namespace maxmod
