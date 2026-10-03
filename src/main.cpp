// max: the MAX messenger (desktop client for Linux) as a Facet module. MAX is
// downloaded from its official repository on the device, runs in this
// module's container on the Wayland display of the facet-wayland module, and
// its window is shown full screen on this module's screen. Its desktop
// notifications become Facet notifications; its login is kept by a small
// Secret Service on a private session bus.
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "facet/plugin.h"
#include "i18n/i18n.h"
#include "installer.h"
#include "notify.h"
#include "secrets.h"
#include "session.h"

using facet::Json;
using facet::sdk::LentSurface;
using facet::sdk::Notification;
using facet::sdk::Plugin;
using facet::sdk::Screen;
using namespace maxmod;

namespace {

#ifndef MAX_MODULE_VERSION
#define MAX_MODULE_VERSION "dev"  // set by CMake
#endif

constexpr double kCheckEvery = 12 * 3600;  // update checks
constexpr int kMaxCrashes = 3;

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string exe_dir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    std::string p = n > 0 ? std::string(buf, size_t(n)) : std::string(".");
    return p.substr(0, p.rfind('/'));
}

bool exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

std::string megabytes(uint64_t bytes) { return std::to_string((bytes + 500000) / 1000000); }

class MaxModule {
public:
    explicit MaxModule(Plugin& plugin)
        : plugin_(plugin), installer_(plugin.data_dir()), secrets_(plugin.data_dir() + "/secrets.json") {
        bridge_.on_notify = [this](const DesktopNotification& n) { post(n); };
        bridge_.on_close = [this](uint32_t id) { plugin_.cancel_notification("m" + std::to_string(id)); };
    }

    void hello() {
        secrets_.load();
        want_run_ = !installer_.installed_version().empty();
        installer_.start(false);  // look for updates
        last_check_ = now_s();
        refresh();
    }

    void shutdown() {
        installer_.cancel();
        unwatch();
        session_.stop();
    }

    void tick() {
        double t = now_s();
        poll_installer();
        int status = 0;
        if (session_.reap(status)) {
            unwatch();
            if (want_run_ && !stopping_) {
                ++crashes_;
                Plugin::log("max: MAX exited (status %d), %d in a row", status, crashes_);
                if (crashes_ > kMaxCrashes) {
                    want_run_ = false;
                    error_ = tr("MAX keeps closing. Start it again or reinstall it.");
                } else {
                    next_start_ = t + std::pow(2.0, crashes_);
                }
            }
            stopping_ = false;
        }
        if (!session_.bus_up()) unwatch();
        if (want_run_ && !session_.running() && !installer_.busy() && t >= next_start_) start(t);
        if (session_.running() && t - started_at_ > 60) crashes_ = 0;  // ran fine for a while
        if (!installer_.busy() && t - last_check_ > kCheckEvery) {
            installer_.start(false);
            last_check_ = t;
        }
        refresh();
    }

    void on_event(const std::string& id) {
        if (id == "install" || id == "update") {
            installer_.start(true);
            error_.clear();
        } else if (id == "cancel") {
            installer_.cancel();
        } else if (id == "start") {
            crashes_ = 0;
            error_.clear();
            want_run_ = true;
            next_start_ = 0;
        } else if (id == "stop") {
            want_run_ = false;
            stop_app();
        } else if (id == "signout") {
            want_run_ = false;
            stop_app();
            secrets_.clear();
            remove_tree(plugin_.data_dir() + "/home");
        } else if (id == "remove") {
            want_run_ = false;
            stop_app();
            installer_.remove_app();
        }
        refresh();
    }

    void on_lent(const std::string& id, bool available) {
        if (available) surface_ = id;
        else if (surface_ == id) surface_.clear();
        refresh();
    }

    void on_visible(bool visible) {
        if (visible) {
            unread_ = 0;
            plugin_.set_badge(0);
        }
        refresh();
    }

    void on_notification_action(const std::string& id, const std::string& action) {
        if (id == "update") {
            if (action != "dismiss") on_event("update");
            return;
        }
        if (id.size() < 2 || id[0] != 'm' || !session_.bus_up()) return;
        uint32_t n = uint32_t(std::strtoul(id.c_str() + 1, nullptr, 10));
        if (action == "dismiss") bridge_.dismissed(session_.bus(), n);
        else bridge_.invoke(session_.bus(), n, action == "open" ? "default" : action);
    }

    void refresh() {
        std::string tile;
        Installer::Status st = installer_.status();
        if (installer_.busy() && st.phase == Installer::Phase::Downloading)
            tile = tr("Downloading {}%", {std::to_string(int(st.progress * 100))});
        else if (installer_.busy() && st.phase != Installer::Phase::Checking) tile = tr("Installing…");
        else if (installer_.installed_version().empty()) tile = tr("Not installed");
        else if (unread_ > 0) tile = tr("{} new", {std::to_string(unread_)});
        plugin_.set_tile(tile);
        if (plugin_.visible()) plugin_.set_ui(build());
    }

private:
    std::string tr(std::string_view k) const { return plugin_.tr(k); }
    std::string tr(std::string_view k, const std::vector<std::string>& a) const { return plugin_.tr(k, a); }

    std::string wayland_socket() const {
        std::string dir = plugin_.endpoint("display.wayland");
        return dir.empty() ? std::string() : dir + "/wayland-0";
    }

    void start(double t) {
        std::string socket = wayland_socket();
        if (socket.empty() || !exists(socket)) return;  // the compositor creates it shortly
        SessionPaths p;
        p.runtime = exe_dir() + "/runtime";
        p.app = installer_.app_dir();
        p.home = plugin_.data_dir() + "/home";
        p.wayland = socket;
        p.bus = "/tmp/facet-max-bus";
        p.locale = plugin_.catalog().language();
        p.gl = plugin_.gl_dir();
        if (exists(plugin_.data_dir() + "/env.override")) p.env_override = plugin_.data_dir() + "/env.override";
        std::string err;
        if (!session_.start(p, err)) {
            Plugin::log("max: %s", err.c_str());
            error_ = err;
            next_start_ = t + 10;
            return;
        }
        started_at_ = t;
        bus_fd_ = session_.bus().fd();
        plugin_.watch_fd(bus_fd_, [this] { on_bus(); });
        Plugin::log("max: started MAX %s", installer_.installed_version().c_str());
    }

    void stop_app() {
        stopping_ = session_.running();
        unwatch();
        session_.stop();
    }

    void unwatch() {
        if (bus_fd_ >= 0) plugin_.unwatch_fd(bus_fd_);
        bus_fd_ = -1;
    }

    void on_bus() {
        std::vector<dbus::Message> msgs;
        if (!session_.bus().read(msgs)) unwatch();
        for (const auto& m : msgs) {
            if (secrets_.handle(session_.bus(), m) || bridge_.handle(session_.bus(), m)) continue;
            if (m.type == dbus::Type::Call && m.destination.compare(0, 1, ":") != 0 &&
                m.destination != "org.freedesktop.DBus")
                session_.bus().reply_error(m, "org.freedesktop.DBus.Error.UnknownObject", m.path);
        }
    }

    void poll_installer() {
        Installer::Status st = installer_.status();
        if (st.phase == Installer::Phase::Done) {
            if (!st.latest.version.empty()) latest_ = st.latest.version;
            if (st.installed_now) {
                Plugin::log("max: installed MAX %s", latest_.c_str());
                plugin_.cancel_notification("update");
                stop_app();  // a running old version restarts as the new one
                want_run_ = true;
                crashes_ = 0;
                next_start_ = 0;
            } else {
                offer_update();
            }
            installer_.acknowledge();
        } else if (st.phase == Installer::Phase::Failed) {
            if (st.error != "cancelled") error_ = st.error;
            Plugin::log("max: %s", st.error.c_str());
            installer_.acknowledge();
        }
    }

    void offer_update() {
        std::string installed = installer_.installed_version();
        if (installed.empty() || latest_.empty() || compare_versions(latest_, installed) <= 0 || offered_ == latest_)
            return;
        offered_ = latest_;
        if (!plugin_.has_permission("notifications")) return;
        Notification n;
        n.id = "update";
        n.kind = "status";
        n.priority = "low";
        n.title = tr("MAX {} is available", {latest_});
        n.body = tr("Download {} MB and restart MAX.", {megabytes(installer_.status().latest.size)});
        n.actions = {{"update", tr("Update")}};
        plugin_.notify(n);
    }

    void post(const DesktopNotification& d) {
        if (!plugin_.visible()) {
            ++unread_;
            plugin_.set_badge(unread_);
        }
        if (!plugin_.has_permission("notifications")) return;
        Notification n;
        n.id = "m" + std::to_string(d.id);
        n.title = d.summary.empty() ? "MAX" : d.summary;
        n.body = d.body;
        n.priority = d.urgency >= 2 ? "high" : d.urgency == 0 ? "low" : "normal";
        for (const auto& [key, label] : d.actions)
            if (key != "default" && n.actions.size() < 3) n.actions.emplace_back(key, label);
        plugin_.notify(n);
    }

    Screen build() const {
        Screen ui("MAX");
        Installer::Status st = installer_.status();
        std::string installed = installer_.installed_version();
        bool busy = installer_.busy() && st.phase != Installer::Phase::Checking;
        // MAX itself, once its first frame is there.
        if (session_.running() && !surface_.empty() && !busy) ui.fullscreen(surface_);

        ui.section(tr("MAX messenger"));
        if (busy) {
            const char* what = st.phase == Installer::Phase::Downloading ? "Downloading"
                               : st.phase == Installer::Phase::Verifying ? "Checking the download"
                                                                         : "Unpacking";
            std::string text = st.phase == Installer::Phase::Downloading
                                   ? tr("{} of {} MB", {megabytes(uint64_t(st.progress * double(st.total))),
                                                        megabytes(st.total)})
                                   : std::to_string(int(st.progress * 100)) + "%";
            ui.level(tr(what), st.progress, text);
            ui.button("cancel", tr("Cancel"));
        } else if (installed.empty()) {
            ui.note(tr("MAX is not installed yet. It is downloaded from download.max.ru, the official site, "
                       "and runs only inside this module."));
            if (st.total) ui.info(tr("Download"), tr("{} MB, about {} MB on disk", {megabytes(st.total), "1100"}));
            ui.button("install", tr("Install MAX"), "primary");
        } else {
            std::string state = session_.running() ? (surface_.empty() ? tr("starting…") : tr("running"))
                                : want_run_      ? tr("waiting for Wayland…")
                                                 : tr("stopped");
            ui.info(tr("State"), state, session_.running() ? "good" : "normal");
            ui.info(tr("Version"), installed);
            if (!latest_.empty() && compare_versions(latest_, installed) > 0) {
                ui.info(tr("Available"), latest_, "warn");
                ui.button("update", tr("Update"), "primary");
            }
            if (!session_.running()) ui.button("start", tr("Start MAX"), "primary");
            else ui.button("stop", tr("Stop MAX"));
        }
        if (!error_.empty()) ui.info(tr("Error"), error_, "bad");
        if (!installed.empty() && !busy) {
            ui.section(tr("Data"));
            ui.button("signout", tr("Sign out and delete messages on this panel"), "danger");
            ui.button("remove", tr("Remove the app (the login stays)"));
        }
        return ui;
    }

    Plugin& plugin_;
    Installer installer_;
    Session session_;
    SecretService secrets_;
    NotificationBridge bridge_;
    std::string surface_, error_, latest_, offered_;
    bool want_run_ = false, stopping_ = false;
    int crashes_ = 0, unread_ = 0, bus_fd_ = -1;
    double next_start_ = 0, started_at_ = 0, last_check_ = 0;
};

}  // namespace

int main() {
    Plugin plugin("max", MAX_MODULE_VERSION);  // keep in sync with manifest.json
    max_module::register_translations(plugin.catalog());
    MaxModule app(plugin);
    plugin.on_hello = [&](const Json&) { app.hello(); };
    plugin.on_event = [&](const std::string& id, const Json&) { app.on_event(id); };
    plugin.on_surface_lent = [&](const std::string& id, const LentSurface&, bool available) {
        app.on_lent(id, available);
    };
    plugin.on_visible = [&](bool v) { app.on_visible(v); };
    plugin.on_notification_action = [&](const std::string& id, const std::string& a) {
        app.on_notification_action(id, a);
    };
    plugin.on_locale = [&](const std::string&) { app.refresh(); };
    plugin.on_tick = [&] { app.tick(); };
    plugin.on_shutdown = [&] { app.shutdown(); };
    return plugin.run(250);
}
