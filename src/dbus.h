// A minimal D-Bus client: the wire format (little endian) and a connection
// to a bus over a Unix socket. Enough to offer services on the app's private
// session bus (secrets, notifications) without linking libdbus.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace maxmod::dbus {

enum class Type : uint8_t { Invalid = 0, Call = 1, Return = 2, Error = 3, Signal = 4 };

// Marshals values; offsets are relative to the start of the body (or header),
// which the protocol aligns to 8.
class Writer {
public:
    void align(size_t n);
    void byte(uint8_t v);
    void boolean(bool v);
    void u32(uint32_t v);
    void i32(int32_t v) { u32(uint32_t(v)); }
    void u64(uint64_t v);
    void str(const std::string& s);   // s and o
    void sig(const std::string& s);   // g
    void bytes(const std::string& s); // ay
    struct Array {
        size_t len_pos, start;
    };
    Array begin_array(size_t elem_align);
    void end_array(const Array& a);
    void begin_struct() { align(8); }
    // A variant: its signature, then the caller writes the value.
    void variant(const std::string& signature) { sig(signature); }
    void strings(const std::vector<std::string>& v);  // as / ao

    std::string data;
};

class Reader {
public:
    Reader(const std::string& data, size_t pos = 0) : d_(data), pos_(pos) {}
    bool ok() const { return ok_; }
    size_t pos() const { return pos_; }
    void align(size_t n);
    uint8_t byte();
    bool boolean() { return u32() != 0; }
    uint32_t u32();
    int32_t i32() { return int32_t(u32()); }
    uint64_t u64();
    std::string str();  // s and o
    std::string sig();
    std::string bytes();  // ay
    // Reads an array's length; returns where it ends. Elements follow.
    size_t array_end(size_t elem_align);
    std::vector<std::string> strings();  // as / ao
    // Skips one complete value of the single complete type `signature`.
    void skip(const std::string& signature);
    // Reads a variant's signature (the value follows).
    std::string variant() { return sig(); }

private:
    bool need(size_t n);
    const std::string& d_;
    size_t pos_;
    bool ok_ = true;
};

// Length of the first single complete type in `signature`, 0 if invalid.
size_t complete_type_length(const std::string& signature, size_t from = 0);

struct Message {
    Type type = Type::Invalid;
    uint8_t flags = 0;
    uint32_t serial = 0, reply_serial = 0;
    std::string path, interface, member, error, destination, sender, signature;
    std::string body;
};

std::string encode(const Message& m, uint32_t serial);
// Bytes of one complete message at the start of `buf` decoded into `out`:
// > 0 consumed, 0 incomplete, -1 malformed.
long decode(const std::string& buf, Message& out);

class Connection {
public:
    ~Connection();
    // Connects, authenticates (EXTERNAL) and says Hello. English error on failure.
    bool connect(const std::string& socket_path, std::string& error);
    void close();
    int fd() const { return fd_; }
    bool connected() const { return fd_ >= 0; }

    uint32_t send(Message m);
    void reply(const Message& call, const std::string& signature, const std::string& body);
    void reply_error(const Message& call, const std::string& name, const std::string& text);
    void signal(const std::string& path, const std::string& interface, const std::string& member,
                const std::string& signature, const std::string& body);
    void request_name(const std::string& name);
    // Reads what is available; false when the bus went away.
    bool read(std::vector<Message>& out);

private:
    int fd_ = -1;
    uint32_t serial_ = 0;
    std::string in_;
};

}  // namespace maxmod::dbus
