#include "secrets.h"

#include <sys/stat.h>

#include <ctime>
#include <functional>
#include <vector>

#include "facet/json.h"

namespace maxmod {

namespace {
constexpr const char* kRoot = "/org/freedesktop/secrets";
constexpr const char* kService = "/org/freedesktop/secrets";
constexpr const char* kCollection = "/org/freedesktop/secrets/collection/login";
constexpr const char* kAliasDefault = "/org/freedesktop/secrets/aliases/default";
constexpr const char* kSessionPrefix = "/org/freedesktop/secrets/session/";
constexpr const char* kNoSuchObject = "org.freedesktop.Secret.Error.NoSuchObject";

std::string to_hex(const std::string& s) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        out.push_back(d[c >> 4]);
        out.push_back(d[c & 15]);
    }
    return out;
}

std::string from_hex(const std::string& s) {
    auto v = [](char c) { return c >= 'a' ? c - 'a' + 10 : c - '0'; };
    std::string out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(char(v(s[i]) * 16 + v(s[i + 1])));
    return out;
}

bool ends_with(const std::string& s, const char* tail) {
    size_t n = std::char_traits<char>::length(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

std::map<std::string, std::string> read_attrs(dbus::Reader& r) {
    std::map<std::string, std::string> a;
    size_t end = r.array_end(8);
    while (r.ok() && r.pos() < end) {
        r.align(8);
        std::string k = r.str();
        a[k] = r.str();
    }
    return a;
}

void write_attrs(dbus::Writer& w, const std::map<std::string, std::string>& a) {
    auto arr = w.begin_array(8);
    for (const auto& [k, v] : a) {
        w.begin_struct();
        w.str(k);
        w.str(v);
    }
    w.end_array(arr);
}

struct Prop {
    const char* name;
    const char* sig;
    std::function<void(dbus::Writer&)> write;
};

const char* kIntrospect =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\"><node/>";
}  // namespace

SecretService::SecretService(std::string store_path) : store_(std::move(store_path)) {}

void SecretService::load() {
    facet::Json j;
    if (!facet::load_json_file(store_, j)) return;
    next_id_ = uint32_t(std::max(1, j["next"].as_int(1)));
    for (const auto& it : j["items"].items()) {
        Item item;
        item.label = it["label"].str();
        for (const auto& [k, v] : it["attributes"].fields()) item.attributes[k] = v.str();
        item.secret = from_hex(it["secret"].str());
        item.content_type = it["content_type"].as_string("text/plain");
        item.created = uint64_t(it["created"].as_number());
        item.modified = uint64_t(it["modified"].as_number());
        uint32_t id = uint32_t(it["id"].as_int());
        if (id) items_[id] = item;
        if (id >= next_id_) next_id_ = id + 1;
    }
}

void SecretService::save() const {
    facet::Json j = facet::Json::object();
    j["next"] = int(next_id_);
    j["items"] = facet::Json::array();
    for (const auto& [id, item] : items_) {
        facet::Json it = facet::Json::object();
        it["id"] = int(id);
        it["label"] = item.label;
        it["attributes"] = facet::Json::object();
        for (const auto& [k, v] : item.attributes) it["attributes"][k] = v;
        it["secret"] = to_hex(item.secret);
        it["content_type"] = item.content_type;
        it["created"] = double(item.created);
        it["modified"] = double(item.modified);
        j["items"].push_back(it);
    }
    std::string tmp = store_ + ".tmp";
    if (facet::save_json_file(tmp, j)) {
        chmod(tmp.c_str(), 0600);
        std::rename(tmp.c_str(), store_.c_str());
    }
}

void SecretService::clear() {
    items_.clear();
    save();
}

std::string SecretService::item_path(uint32_t id) const { return std::string(kCollection) + "/" + std::to_string(id); }

bool SecretService::item_of(const std::string& path, uint32_t& id) const {
    for (const char* base : {kCollection, kAliasDefault}) {
        std::string prefix = std::string(base) + "/";
        if (path.compare(0, prefix.size(), prefix) == 0) {
            id = uint32_t(std::strtoul(path.c_str() + prefix.size(), nullptr, 10));
            return items_.count(id) > 0;
        }
    }
    return false;
}

bool SecretService::is_collection(const std::string& path) const {
    return path == kCollection || path == kAliasDefault || path == "/org/freedesktop/secrets/aliases/login";
}

std::vector<std::string> SecretService::search(const Attrs& query) const {
    std::vector<std::string> out;
    for (const auto& [id, item] : items_) {
        bool match = true;
        for (const auto& [k, v] : query) {
            auto it = item.attributes.find(k);
            if (it == item.attributes.end() || it->second != v) match = false;
        }
        if (match) out.push_back(item_path(id));
    }
    return out;
}

void SecretService::write_secret(dbus::Writer& w, uint32_t id, const std::string& session) const {
    const Item& item = items_.at(id);
    w.begin_struct();
    w.str(session);
    w.bytes("");  // parameters: none for "plain"
    w.bytes(item.secret);
    w.str(item.content_type);
}

std::string SecretService::props_of(const std::string& path, const std::string& interface, const std::string& name,
                                    bool all) const {
    std::vector<Prop> props;
    uint32_t id = 0;
    if (path == kService) {
        props.push_back({"Collections", "ao", [](dbus::Writer& w) { w.strings({kCollection}); }});
    } else if (is_collection(path)) {
        std::vector<std::string> paths;
        for (const auto& [i, item] : items_) paths.push_back(item_path(i));
        props.push_back({"Items", "ao", [paths](dbus::Writer& w) { w.strings(paths); }});
        props.push_back({"Label", "s", [](dbus::Writer& w) { w.str("Login"); }});
        props.push_back({"Locked", "b", [](dbus::Writer& w) { w.boolean(false); }});
        props.push_back({"Created", "t", [](dbus::Writer& w) { w.u64(0); }});
        props.push_back({"Modified", "t", [](dbus::Writer& w) { w.u64(0); }});
    } else if (item_of(path, id)) {
        const Item& item = items_.at(id);
        props.push_back({"Locked", "b", [](dbus::Writer& w) { w.boolean(false); }});
        props.push_back({"Attributes", "a{ss}", [&item](dbus::Writer& w) { write_attrs(w, item.attributes); }});
        props.push_back({"Label", "s", [&item](dbus::Writer& w) { w.str(item.label); }});
        props.push_back({"Created", "t", [&item](dbus::Writer& w) { w.u64(item.created); }});
        props.push_back({"Modified", "t", [&item](dbus::Writer& w) { w.u64(item.modified); }});
    }
    (void)interface;
    dbus::Writer w;
    if (all) {
        auto arr = w.begin_array(8);
        for (const auto& p : props) {
            w.begin_struct();
            w.str(p.name);
            w.variant(p.sig);
            p.write(w);
        }
        w.end_array(arr);
        return w.data;
    }
    for (const auto& p : props) {
        if (name == p.name) {
            w.variant(p.sig);
            p.write(w);
            return w.data;
        }
    }
    return {};
}

void SecretService::properties(dbus::Connection& bus, const dbus::Message& m) {
    dbus::Reader r(m.body);
    std::string interface = r.str();
    if (m.member == "GetAll") {
        bus.reply(m, "a{sv}", props_of(m.path, interface, {}, true));
    } else if (m.member == "Get") {
        std::string name = r.str();
        std::string v = props_of(m.path, interface, name, false);
        if (v.empty()) bus.reply_error(m, "org.freedesktop.DBus.Error.UnknownProperty", name);
        else bus.reply(m, "v", v);
    } else if (m.member == "Set") {
        std::string name = r.str();
        std::string sig = r.variant();
        uint32_t id = 0;
        if (item_of(m.path, id)) {
            Item& item = items_[id];
            if (name == "Label" && sig == "s") item.label = r.str();
            else if (name == "Attributes" && sig == "a{ss}") item.attributes = read_attrs(r);
            item.modified = uint64_t(std::time(nullptr));
            save();
        }
        bus.reply(m, "", "");
    }
}

bool SecretService::handle(dbus::Connection& bus, const dbus::Message& m) {
    if (m.type != dbus::Type::Call || m.path.compare(0, std::string(kRoot).size(), kRoot) != 0) return false;
    const std::string& f = m.member;
    if (m.interface == "org.freedesktop.DBus.Properties") {
        properties(bus, m);
        return true;
    }
    if (m.interface == "org.freedesktop.DBus.Introspectable") {
        dbus::Writer w;
        w.str(kIntrospect);
        bus.reply(m, "s", w.data);
        return true;
    }
    if (m.interface == "org.freedesktop.DBus.Peer") {
        bus.reply(m, "", "");
        return true;
    }
    dbus::Reader r(m.body);
    dbus::Writer w;
    uint32_t id = 0;

    if (m.path == kService) {
        if (f == "OpenSession") {
            std::string algorithm = r.str();
            if (algorithm != "plain") {
                bus.reply_error(m, "org.freedesktop.DBus.Error.NotSupported", "only plain sessions");
                return true;
            }
            w.variant("s");
            w.str("");
            w.str(kSessionPrefix + std::to_string(next_session_++));
            bus.reply(m, "vo", w.data);
        } else if (f == "CreateCollection") {
            w.str(kCollection);
            w.str("/");
            bus.reply(m, "oo", w.data);
        } else if (f == "SearchItems") {
            w.strings(search(read_attrs(r)));
            w.strings({});
            bus.reply(m, "aoao", w.data);
        } else if (f == "Unlock") {
            w.strings(r.strings());
            w.str("/");
            bus.reply(m, "aoo", w.data);
        } else if (f == "Lock") {
            w.strings({});
            w.str("/");
            bus.reply(m, "aoo", w.data);
        } else if (f == "GetSecrets") {
            std::vector<std::string> paths = r.strings();
            std::string session = r.str();
            auto arr = w.begin_array(8);
            for (const auto& p : paths) {
                if (!item_of(p, id)) continue;
                w.begin_struct();
                w.str(item_path(id));
                write_secret(w, id, session);
            }
            w.end_array(arr);
            bus.reply(m, "a{o(oayays)}", w.data);
        } else if (f == "ReadAlias") {
            std::string alias = r.str();
            w.str(alias == "default" || alias == "login" ? kCollection : "/");
            bus.reply(m, "o", w.data);
        } else if (f == "SetAlias") {
            bus.reply(m, "", "");
        } else {
            bus.reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", f);
        }
        return true;
    }

    if (is_collection(m.path)) {
        if (f == "SearchItems") {
            w.strings(search(read_attrs(r)));
            bus.reply(m, "ao", w.data);
        } else if (f == "CreateItem") {
            Item item;
            size_t end = r.array_end(8);
            while (r.ok() && r.pos() < end) {
                r.align(8);
                std::string key = r.str();
                std::string sig = r.variant();
                if (ends_with(key, ".Label") && sig == "s") item.label = r.str();
                else if (ends_with(key, ".Attributes") && sig == "a{ss}") item.attributes = read_attrs(r);
                else r.skip(sig);
            }
            r.align(8);
            r.str();  // session
            r.bytes();  // parameters
            item.secret = r.bytes();
            item.content_type = r.str();
            bool replace = r.boolean();
            if (!r.ok()) {
                bus.reply_error(m, "org.freedesktop.DBus.Error.InvalidArgs", "bad item");
                return true;
            }
            item.created = item.modified = uint64_t(std::time(nullptr));
            uint32_t target = 0;
            if (replace) {
                for (const auto& [i, existing] : items_)
                    if (existing.attributes == item.attributes) target = i;
            }
            if (target) item.created = items_[target].created;
            else target = next_id_++;
            items_[target] = item;
            save();
            w.str(item_path(target));
            w.str("/");
            bus.reply(m, "oo", w.data);
            dbus::Writer sig;
            sig.str(item_path(target));
            bus.signal(kCollection, "org.freedesktop.Secret.Collection", "ItemCreated", "o", sig.data);
        } else if (f == "Delete") {
            clear();
            w.str("/");
            bus.reply(m, "o", w.data);
        } else {
            bus.reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", f);
        }
        return true;
    }

    if (item_of(m.path, id)) {
        if (f == "GetSecret") {
            write_secret(w, id, r.str());
            bus.reply(m, "(oayays)", w.data);
        } else if (f == "SetSecret") {
            r.align(8);
            r.str();
            r.bytes();
            Item& item = items_[id];
            item.secret = r.bytes();
            item.content_type = r.str();
            item.modified = uint64_t(std::time(nullptr));
            save();
            bus.reply(m, "", "");
        } else if (f == "Delete") {
            items_.erase(id);
            save();
            w.str("/");
            bus.reply(m, "o", w.data);
        } else {
            bus.reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", f);
        }
        return true;
    }

    if (m.path.compare(0, std::string(kSessionPrefix).size(), kSessionPrefix) == 0) {
        bus.reply(m, "", "");  // Close
        return true;
    }
    bus.reply_error(m, kNoSuchObject, m.path);
    return true;
}

}  // namespace maxmod
