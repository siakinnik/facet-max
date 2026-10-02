// org.freedesktop.Notifications on the app's private session bus: what MAX
// posts as desktop notifications becomes Facet notifications, and taps or
// buttons on them go back to MAX as ActionInvoked.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "dbus.h"

namespace maxmod {

struct DesktopNotification {
    uint32_t id = 0;
    std::string app, summary, body;  // body without markup
    std::vector<std::pair<std::string, std::string>> actions;  // key, label ("default" = tap)
    int urgency = 1;  // 0 low, 1 normal, 2 critical
};

class NotificationBridge {
public:
    std::function<void(const DesktopNotification&)> on_notify;
    std::function<void(uint32_t id)> on_close;

    bool handle(dbus::Connection& bus, const dbus::Message& m);
    // The user tapped the notification ("default") or pressed a button.
    void invoke(dbus::Connection& bus, uint32_t id, const std::string& action);
    void dismissed(dbus::Connection& bus, uint32_t id);

private:
    uint32_t next_ = 1;
};

// Notification body markup (<b>, <i>, <a>, entities) as plain text.
std::string strip_markup(const std::string& s);

}  // namespace maxmod
