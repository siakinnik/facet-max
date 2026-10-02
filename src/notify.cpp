#include "notify.h"

namespace maxmod {

namespace {
constexpr const char* kPath = "/org/freedesktop/Notifications";
constexpr const char* kInterface = "org.freedesktop.Notifications";
}  // namespace

std::string strip_markup(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '<') {
            size_t end = s.find('>', i);
            if (end == std::string::npos) break;
            i = end;
            continue;
        }
        if (s[i] == '&') {
            static const std::pair<const char*, char> entities[] = {
                {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&#39;", '\''}};
            bool done = false;
            for (const auto& [name, c] : entities) {
                size_t n = std::char_traits<char>::length(name);
                if (s.compare(i, n, name) == 0) {
                    out.push_back(c);
                    i += n - 1;
                    done = true;
                    break;
                }
            }
            if (done) continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

bool NotificationBridge::handle(dbus::Connection& bus, const dbus::Message& m) {
    if (m.type != dbus::Type::Call || m.path != kPath) return false;
    dbus::Reader r(m.body);
    dbus::Writer w;
    if (m.interface == "org.freedesktop.DBus.Properties") {
        w.end_array(w.begin_array(8));  // no properties
        bus.reply(m, "a{sv}", w.data);
        return true;
    }
    if (m.member == "GetCapabilities") {
        w.strings({"body", "actions", "persistence"});
        bus.reply(m, "as", w.data);
    } else if (m.member == "GetServerInformation") {
        w.str("Facet");
        w.str("facet");
        w.str("0.1");
        w.str("1.2");
        bus.reply(m, "ssss", w.data);
    } else if (m.member == "Notify") {
        DesktopNotification n;
        n.app = r.str();
        uint32_t replaces = r.u32();
        r.str();  // icon
        n.summary = r.str();
        n.body = strip_markup(r.str());
        std::vector<std::string> actions = r.strings();
        for (size_t i = 0; i + 1 < actions.size(); i += 2) n.actions.emplace_back(actions[i], actions[i + 1]);
        size_t end = r.array_end(8);
        while (r.ok() && r.pos() < end) {
            r.align(8);
            std::string key = r.str();
            std::string sig = r.variant();
            if (key == "urgency" && sig == "y") n.urgency = r.byte();
            else r.skip(sig);
        }
        if (!r.ok()) {
            bus.reply_error(m, "org.freedesktop.DBus.Error.InvalidArgs", "bad notification");
            return true;
        }
        n.id = replaces ? replaces : next_++;
        w.u32(n.id);
        bus.reply(m, "u", w.data);
        if (on_notify) on_notify(n);
    } else if (m.member == "CloseNotification") {
        uint32_t id = r.u32();
        if (on_close) on_close(id);
        w.u32(id);
        w.u32(3);  // closed by a call to CloseNotification
        bus.signal(kPath, kInterface, "NotificationClosed", "uu", w.data);
        bus.reply(m, "", "");
    } else {
        bus.reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", m.member);
    }
    return true;
}

void NotificationBridge::invoke(dbus::Connection& bus, uint32_t id, const std::string& action) {
    dbus::Writer w;
    w.u32(id);
    w.str(action);
    bus.signal(kPath, kInterface, "ActionInvoked", "us", w.data);
    dismissed(bus, id);
}

void NotificationBridge::dismissed(dbus::Connection& bus, uint32_t id) {
    dbus::Writer w;
    w.u32(id);
    w.u32(2);  // dismissed by the user
    bus.signal(kPath, kInterface, "NotificationClosed", "uu", w.data);
}

}  // namespace maxmod
