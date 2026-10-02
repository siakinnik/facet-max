#include "dbus.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace maxmod::dbus {

// ---------------------------------------------------------------- Writer

void Writer::align(size_t n) {
    while (data.size() % n) data.push_back('\0');
}

void Writer::byte(uint8_t v) { data.push_back(char(v)); }

void Writer::boolean(bool v) { u32(v ? 1 : 0); }

void Writer::u32(uint32_t v) {
    align(4);
    for (int i = 0; i < 4; ++i) data.push_back(char((v >> (8 * i)) & 0xff));
}

void Writer::u64(uint64_t v) {
    align(8);
    for (int i = 0; i < 8; ++i) data.push_back(char((v >> (8 * i)) & 0xff));
}

void Writer::str(const std::string& s) {
    u32(uint32_t(s.size()));
    data += s;
    data.push_back('\0');
}

void Writer::sig(const std::string& s) {
    byte(uint8_t(s.size()));
    data += s;
    data.push_back('\0');
}

void Writer::bytes(const std::string& s) {
    u32(uint32_t(s.size()));
    data += s;
}

Writer::Array Writer::begin_array(size_t elem_align) {
    u32(0);
    Array a{data.size() - 4, 0};
    align(elem_align);
    a.start = data.size();
    return a;
}

void Writer::end_array(const Array& a) {
    uint32_t len = uint32_t(data.size() - a.start);
    for (int i = 0; i < 4; ++i) data[a.len_pos + size_t(i)] = char((len >> (8 * i)) & 0xff);
}

void Writer::strings(const std::vector<std::string>& v) {
    Array a = begin_array(4);
    for (const auto& s : v) str(s);
    end_array(a);
}

// ---------------------------------------------------------------- Reader

bool Reader::need(size_t n) {
    if (!ok_ || pos_ + n > d_.size()) {
        ok_ = false;
        return false;
    }
    return true;
}

void Reader::align(size_t n) {
    size_t p = (pos_ + n - 1) / n * n;
    if (p > d_.size()) ok_ = false;
    else pos_ = p;
}

uint8_t Reader::byte() {
    if (!need(1)) return 0;
    return uint8_t(d_[pos_++]);
}

uint32_t Reader::u32() {
    align(4);
    if (!need(4)) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(uint8_t(d_[pos_ + size_t(i)])) << (8 * i);
    pos_ += 4;
    return v;
}

uint64_t Reader::u64() {
    align(8);
    if (!need(8)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(uint8_t(d_[pos_ + size_t(i)])) << (8 * i);
    pos_ += 8;
    return v;
}

std::string Reader::str() {
    uint32_t n = u32();
    if (!need(size_t(n) + 1)) return {};
    std::string s = d_.substr(pos_, n);
    pos_ += size_t(n) + 1;
    return s;
}

std::string Reader::sig() {
    uint8_t n = byte();
    if (!need(size_t(n) + 1)) return {};
    std::string s = d_.substr(pos_, n);
    pos_ += size_t(n) + 1;
    return s;
}

std::string Reader::bytes() {
    uint32_t n = u32();
    if (!need(n)) return {};
    std::string s = d_.substr(pos_, n);
    pos_ += n;
    return s;
}

size_t Reader::array_end(size_t elem_align) {
    uint32_t n = u32();
    align(elem_align);
    if (!ok_ || pos_ + n > d_.size()) {
        ok_ = false;
        return pos_;
    }
    return pos_ + n;
}

std::vector<std::string> Reader::strings() {
    std::vector<std::string> v;
    size_t end = array_end(4);
    while (ok_ && pos_ < end) v.push_back(str());
    return v;
}

size_t complete_type_length(const std::string& s, size_t i) {
    if (i >= s.size()) return 0;
    char c = s[i];
    if (std::strchr("ybnqiuxtdsogvh", c)) return 1;
    if (c == 'a') {
        size_t n = complete_type_length(s, i + 1);
        return n ? n + 1 : 0;
    }
    if (c == '(' || c == '{') {
        char close = c == '(' ? ')' : '}';
        size_t j = i + 1;
        while (j < s.size() && s[j] != close) {
            size_t n = complete_type_length(s, j);
            if (!n) return 0;
            j += n;
        }
        return j < s.size() ? j - i + 1 : 0;
    }
    return 0;
}

namespace {
size_t alignment_of(char c) {
    switch (c) {
        case 'y': case 'g': case 'v': return 1;
        case 'n': case 'q': return 2;
        case 'x': case 't': case 'd': case '(': case '{': return 8;
        default: return 4;
    }
}
}  // namespace

void Reader::skip(const std::string& s) {
    if (s.empty() || !ok_) return;
    char c = s[0];
    switch (c) {
        case 'y': byte(); return;
        case 'n': case 'q':
            align(2);
            if (need(2)) pos_ += 2;
            return;
        case 'b': case 'i': case 'u': case 'h': u32(); return;
        case 'x': case 't': case 'd': u64(); return;
        case 's': case 'o': str(); return;
        case 'g': sig(); return;
        case 'v': {
            std::string inner = sig();
            if (complete_type_length(inner) != inner.size() || inner.empty()) {
                ok_ = false;
                return;
            }
            skip(inner);
            return;
        }
        case 'a': {
            std::string elem = s.substr(1, complete_type_length(s, 1));
            size_t end = array_end(alignment_of(elem.empty() ? 'y' : elem[0]));
            if (ok_) pos_ = end;
            return;
        }
        case '(': case '{': {
            align(8);
            size_t j = 1;
            while (ok_ && j + 1 < s.size()) {
                size_t n = complete_type_length(s, j);
                if (!n) {
                    ok_ = false;
                    return;
                }
                skip(s.substr(j, n));
                j += n;
            }
            return;
        }
        default: ok_ = false;
    }
}

// ---------------------------------------------------------------- messages

namespace {
enum Field : uint8_t { kPath = 1, kInterface, kMember, kErrorName, kReplySerial, kDestination, kSender, kSignature };

void field(Writer& w, uint8_t code, const char* sig, const std::string& value) {
    w.begin_struct();
    w.byte(code);
    w.sig(sig);
    if (sig[0] == 'g') w.sig(value);
    else w.str(value);
}
}  // namespace

std::string encode(const Message& m, uint32_t serial) {
    Writer w;
    w.byte('l');
    w.byte(uint8_t(m.type));
    w.byte(m.flags);
    w.byte(1);
    w.u32(uint32_t(m.body.size()));
    w.u32(serial);
    Writer::Array a = w.begin_array(8);
    if (!m.path.empty()) field(w, kPath, "o", m.path);
    if (!m.interface.empty()) field(w, kInterface, "s", m.interface);
    if (!m.member.empty()) field(w, kMember, "s", m.member);
    if (!m.error.empty()) field(w, kErrorName, "s", m.error);
    if (m.reply_serial) {
        w.begin_struct();
        w.byte(kReplySerial);
        w.sig("u");
        w.u32(m.reply_serial);
    }
    if (!m.destination.empty()) field(w, kDestination, "s", m.destination);
    if (!m.signature.empty()) field(w, kSignature, "g", m.signature);
    w.end_array(a);
    w.align(8);
    return w.data + m.body;
}

long decode(const std::string& buf, Message& out) {
    if (buf.size() < 16) return 0;
    if (buf[0] != 'l' || buf[3] != 1) return -1;  // big endian peers do not exist here
    Reader r(buf, 4);
    uint32_t body_len = r.u32();
    uint32_t serial = r.u32();
    uint32_t fields_len = r.u32();
    if (body_len > (1u << 27) || fields_len > (1u << 26)) return -1;
    size_t header_end = (16 + size_t(fields_len) + 7) / 8 * 8;
    size_t total = header_end + body_len;
    if (buf.size() < total) return 0;
    out = Message{};
    out.type = Type(uint8_t(buf[1]));
    out.flags = uint8_t(buf[2]);
    out.serial = serial;
    std::string header = buf.substr(0, 16 + fields_len);
    Reader h(header, 16);
    while (h.ok() && h.pos() < header.size()) {
        h.align(8);
        if (h.pos() >= header.size()) break;
        uint8_t code = h.byte();
        std::string sig = h.sig();
        if (sig == "s" || sig == "o") {
            std::string v = h.str();
            switch (code) {
                case kPath: out.path = v; break;
                case kInterface: out.interface = v; break;
                case kMember: out.member = v; break;
                case kErrorName: out.error = v; break;
                case kDestination: out.destination = v; break;
                case kSender: out.sender = v; break;
                default: break;
            }
        } else if (sig == "g") {
            std::string v = h.sig();
            if (code == kSignature) out.signature = v;
        } else if (sig == "u") {
            uint32_t v = h.u32();
            if (code == kReplySerial) out.reply_serial = v;
        } else {
            h.skip(sig);
        }
    }
    if (!h.ok()) return -1;
    out.body = buf.substr(header_end, body_len);
    return long(total);
}

// ---------------------------------------------------------------- connection

Connection::~Connection() { close(); }

void Connection::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    in_.clear();
}

namespace {
bool write_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                pollfd p{fd, POLLOUT, 0};
                ::poll(&p, 1, 1000);
                continue;
            }
            return false;
        }
        off += size_t(n);
    }
    return true;
}

bool read_line(int fd, std::string& line) {
    line.clear();
    char c;
    while (line.size() < 4096) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 5000) <= 0) return false;
        ssize_t n = ::recv(fd, &c, 1, 0);
        if (n <= 0) return false;
        line.push_back(c);
        if (line.size() >= 2 && line.compare(line.size() - 2, 2, "\r\n") == 0) return true;
    }
    return false;
}
}  // namespace

bool Connection::connect(const std::string& socket_path, std::string& error) {
    close();
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof addr.sun_path) {
        error = "socket path too long";
        return false;
    }
    std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size() + 1);
    fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0 || ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        error = std::string("cannot connect to the bus: ") + std::strerror(errno);
        close();
        return false;
    }
    // EXTERNAL: the uid in decimal, hex-encoded.
    std::string uid = std::to_string(getuid()), hex;
    for (char c : uid) {
        char b[3];
        std::snprintf(b, sizeof b, "%02x", unsigned(uint8_t(c)));
        hex += b;
    }
    std::string line;
    if (!write_all(fd_, std::string(1, '\0') + "AUTH EXTERNAL " + hex + "\r\n") || !read_line(fd_, line) ||
        line.compare(0, 3, "OK ") != 0 || !write_all(fd_, "BEGIN\r\n")) {
        error = "bus authentication failed";
        close();
        return false;
    }
    fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
    Message hello;
    hello.type = Type::Call;
    hello.path = "/org/freedesktop/DBus";
    hello.interface = "org.freedesktop.DBus";
    hello.member = "Hello";
    hello.destination = "org.freedesktop.DBus";
    send(hello);
    return true;
}

uint32_t Connection::send(Message m) {
    if (fd_ < 0) return 0;
    uint32_t serial = ++serial_;
    if (!write_all(fd_, encode(m, serial))) close();
    return serial;
}

void Connection::reply(const Message& call, const std::string& signature, const std::string& body) {
    if (call.flags & 0x1) return;  // NO_REPLY_EXPECTED
    Message m;
    m.type = Type::Return;
    m.reply_serial = call.serial;
    m.destination = call.sender;
    m.signature = signature;
    m.body = body;
    send(m);
}

void Connection::reply_error(const Message& call, const std::string& name, const std::string& text) {
    if (call.flags & 0x1) return;
    Message m;
    m.type = Type::Error;
    m.reply_serial = call.serial;
    m.destination = call.sender;
    m.error = name;
    m.signature = "s";
    Writer w;
    w.str(text);
    m.body = w.data;
    send(m);
}

void Connection::signal(const std::string& path, const std::string& interface, const std::string& member,
                        const std::string& signature, const std::string& body) {
    Message m;
    m.type = Type::Signal;
    m.path = path;
    m.interface = interface;
    m.member = member;
    m.signature = signature;
    m.body = body;
    send(m);
}

void Connection::request_name(const std::string& name) {
    Message m;
    m.type = Type::Call;
    m.path = "/org/freedesktop/DBus";
    m.interface = "org.freedesktop.DBus";
    m.member = "RequestName";
    m.destination = "org.freedesktop.DBus";
    m.signature = "su";
    Writer w;
    w.str(name);
    w.u32(0x4);  // DO_NOT_QUEUE
    m.body = w.data;
    send(m);
}

bool Connection::read(std::vector<Message>& out) {
    if (fd_ < 0) return false;
    char buf[65536];
    for (;;) {
        ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
        if (n > 0) {
            in_.append(buf, size_t(n));
            continue;
        }
        if (n == 0) {
            close();
            return false;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        close();
        return false;
    }
    for (;;) {
        Message m;
        long used = decode(in_, m);
        if (used == 0) break;
        if (used < 0) {
            close();
            return false;
        }
        in_.erase(0, size_t(used));
        out.push_back(std::move(m));
    }
    return true;
}

}  // namespace maxmod::dbus
