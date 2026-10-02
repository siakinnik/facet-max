// org.freedesktop.secrets for MAX on its private session bus: MAX keeps its
// login token through libsecret and refuses to start without a Secret
// Service. One collection ("login", also the "default" alias), "plain"
// sessions only, stored in a file in the module's data directory, which only
// this module's container can read.
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "dbus.h"

namespace maxmod {

class SecretService {
public:
    explicit SecretService(std::string store_path);
    void load();
    // True if the message was for the Secret Service (and was answered).
    bool handle(dbus::Connection& bus, const dbus::Message& m);
    size_t items() const { return items_.size(); }
    void clear();  // forget everything (sign out)

private:
    struct Item {
        std::string label;
        std::map<std::string, std::string> attributes;
        std::string secret, content_type;
        uint64_t created = 0, modified = 0;
    };
    using Attrs = std::map<std::string, std::string>;

    void save() const;
    std::string item_path(uint32_t id) const;
    bool item_of(const std::string& path, uint32_t& id) const;
    bool is_collection(const std::string& path) const;
    std::vector<std::string> search(const Attrs& query) const;
    void write_secret(dbus::Writer& w, uint32_t id, const std::string& session) const;
    void properties(dbus::Connection& bus, const dbus::Message& m);
    std::string props_of(const std::string& path, const std::string& interface, const std::string& name,
                         bool all) const;

    std::string store_;
    std::map<uint32_t, Item> items_;
    uint32_t next_id_ = 1, next_session_ = 1;
};

}  // namespace maxmod
