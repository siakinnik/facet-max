// D-Bus marshalling, the package index and version order, and unpacking
// (including refusing unsafe paths).
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "dbus.h"
#include "installer.h"
#include "notify.h"
#include "package.h"

using namespace maxmod;

static int failures = 0;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                             \
        }                                                           \
    } while (0)

static void test_dbus() {
    CHECK(dbus::complete_type_length("a{sv}i") == 5);
    CHECK(dbus::complete_type_length("(oayays)") == 8);
    CHECK(dbus::complete_type_length("a{sv") == 0);

    dbus::Writer w;
    w.str("hello");
    auto a = w.begin_array(8);
    w.begin_struct();
    w.str("urgency");
    w.variant("y");
    w.byte(2);
    w.begin_struct();
    w.str("list");
    w.variant("as");
    w.strings({"x", "y"});
    w.end_array(a);
    w.i32(-1);

    dbus::Message m;
    m.type = dbus::Type::Call;
    m.path = "/org/freedesktop/Notifications";
    m.interface = "org.freedesktop.Notifications";
    m.member = "Notify";
    m.destination = "org.freedesktop.Notifications";
    m.signature = "sa{sv}i";
    m.body = w.data;
    std::string wire = dbus::encode(m, 7);
    dbus::Message out;
    CHECK(dbus::decode(wire.substr(0, 10), out) == 0);  // incomplete
    CHECK(dbus::decode(wire, out) == long(wire.size()));
    CHECK(out.serial == 7 && out.member == "Notify" && out.signature == "sa{sv}i" && out.path == m.path);

    dbus::Reader r(out.body);
    CHECK(r.str() == "hello");
    size_t end = r.array_end(8);
    int urgency = -1;
    while (r.ok() && r.pos() < end) {
        r.align(8);
        std::string k = r.str();
        std::string sig = r.variant();
        if (k == "urgency") urgency = r.byte();
        else r.skip(sig);
    }
    CHECK(urgency == 2);
    CHECK(r.i32() == -1);
    CHECK(r.ok());

    // Empty dicts still pad to the first element's alignment.
    dbus::Writer e;
    e.end_array(e.begin_array(8));
    CHECK(e.data.size() == 8);
}

static void test_index() {
    const char* index =
        "Package: max\nVersion: 26.33.0\nArchitecture: amd64\nFilename: pool/m/max/MAX-26.33.0.deb\nSize: 10\n"
        "SHA256: 1111111111111111111111111111111111111111111111111111111111111111\nDescription: x\n continued\n\n"
        "Package: max\nVersion: 26.34.0\nArchitecture: amd64\nFilename: pool/m/max/MAX-26.34.0.deb\nSize: 20\n"
        "SHA256: 2222222222222222222222222222222222222222222222222222222222222222\n\n"
        "Package: max\nVersion: 26.4.0\nArchitecture: amd64\nFilename: pool/m/max/MAX-26.4.0.deb\nSize: 30\n"
        "SHA256: 3333333333333333333333333333333333333333333333333333333333333333\n\n"
        "Package: other\nVersion: 99\nArchitecture: amd64\nFilename: o.deb\nSize: 1\n"
        "SHA256: 4444444444444444444444444444444444444444444444444444444444444444\n";
    PackageInfo p;
    CHECK(parse_packages(index, "max", "amd64", p));
    CHECK(p.version == "26.34.0" && p.size == 20 && p.filename == "pool/m/max/MAX-26.34.0.deb");
    CHECK(!parse_packages(index, "max", "arm64", p));

    CHECK(compare_versions("26.34.0", "26.4.0") > 0);
    CHECK(compare_versions("1.0~rc1", "1.0") < 0);
    CHECK(compare_versions("1:1.0", "2.0") > 0);
    CHECK(compare_versions("1.0-2", "1.0-10") < 0);
    CHECK(compare_versions("1.0", "1.0") == 0);
}

// A ustar header for the test archive.
static std::string tar_header(const std::string& name, char type, size_t size, const std::string& link = {}) {
    std::string h(512, '\0');
    std::memcpy(&h[0], name.data(), std::min<size_t>(name.size(), 100));
    std::snprintf(&h[100], 8, "%07o", type == '5' ? 0755u : 0644u);
    std::snprintf(&h[124], 12, "%011zo", size);
    std::memcpy(&h[148], "        ", 8);
    h[156] = type;
    std::memcpy(&h[157], link.data(), std::min<size_t>(link.size(), 100));
    std::memcpy(&h[257], "ustar", 5);
    unsigned sum = 0;
    for (unsigned char c : h) sum += c;
    std::snprintf(&h[148], 8, "%06o", sum);
    h[155] = ' ';
    return h;
}

static std::string tar_entry(const std::string& name, char type, const std::string& data, const std::string& link = {}) {
    std::string e = tar_header(name, type, data.size(), link) + data;
    e.append((512 - data.size() % 512) % 512, '\0');
    return e;
}

static std::string read_file(const std::string& p) {
    std::ifstream f(p);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

static void test_tar() {
    char tmpl[] = "/tmp/maxtestXXXXXX";
    std::string dir = mkdtemp(tmpl);
    std::string long_name = "./usr/share/max/" + std::string(120, 'n') + ".txt";
    std::string archive = tar_entry("./", '5', "") + tar_entry("./usr/share/max/", '5', "") +
                          tar_entry("./usr/share/max/bin/max", '0', "#!binary") +
                          tar_entry("./usr/share/max/lib64/libA.so.1.0", '0', "lib") +
                          tar_entry("./usr/share/max/lib64/libA.so.1", '2', "", "libA.so.1.0") +
                          tar_entry("./usr/share/max/plugins/x.so", '2', "", "../lib64/libA.so.1") +
                          tar_entry("././@LongLink", 'L', long_name + std::string(1, '\0')) +
                          tar_entry("./usr/share/max/truncated", '0', "long") +
                          tar_entry("./usr/bin/other", '0', "skip me") + std::string(1024, '\0');
    TarReader tar("usr/share/max/", dir + "/app");
    for (size_t i = 0; i < archive.size(); i += 37) CHECK(tar.feed(archive.data() + i, std::min<size_t>(37, archive.size() - i)));
    CHECK(tar.finished());
    CHECK(read_file(dir + "/app/bin/max") == "#!binary");
    CHECK(read_file(dir + "/app/lib64/libA.so.1") == "lib");
    CHECK(read_file(dir + "/app/plugins/x.so") == "lib");
    CHECK(read_file(dir + "/app/" + std::string(120, 'n') + ".txt") == "long");
    struct stat st;
    CHECK(::stat((dir + "/app/usr").c_str(), &st) != 0);  // outside the prefix: not unpacked
    CHECK(tar.files() == 3);

    TarReader evil1("usr/share/max/", dir + "/evil1");
    std::string a1 = tar_entry("./usr/share/max/../../../etc/x", '0', "x");
    CHECK(!evil1.feed(a1.data(), a1.size()));
    TarReader evil2("usr/share/max/", dir + "/evil2");
    std::string a2 = tar_entry("./usr/share/max/up", '2', "", "../../../../etc");
    CHECK(!evil2.feed(a2.data(), a2.size()));
    TarReader evil3("usr/share/max/", dir + "/evil3");
    std::string a3 = tar_entry("./usr/share/max/abs", '2', "", "/etc/passwd");
    CHECK(!evil3.feed(a3.data(), a3.size()));
    remove_tree(dir);
}

static void test_markup() {
    CHECK(strip_markup("<b>Anna</b>: hi &amp; bye &lt;3") == "Anna: hi & bye <3");
    CHECK(strip_markup("plain") == "plain");
}

int main() {
    test_dbus();
    test_index();
    test_tar();
    test_markup();
    if (failures) return 1;
    std::printf("ok: max tests passed\n");
    return 0;
}
