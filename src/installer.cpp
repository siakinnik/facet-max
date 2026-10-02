#include "installer.h"

#include <dirent.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <vector>

#include "https.h"

namespace maxmod {

namespace {
int remove_entry(const char* path, const struct stat*, int, struct FTW*) {
    ::remove(path);
    return 0;
}

std::string read_link(const std::string& path) {
    char buf[4096];
    ssize_t n = ::readlink(path.c_str(), buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, size_t(n)) : std::string();
}
}  // namespace

void remove_tree(const std::string& path) { nftw(path.c_str(), remove_entry, 32, FTW_DEPTH | FTW_PHYS); }

Installer::Installer(std::string data_dir) : data_(std::move(data_dir)) {}

Installer::~Installer() {
    cancel();
    if (worker_.joinable()) worker_.join();
}

std::string Installer::app_dir() const { return data_ + "/app/current"; }

std::string Installer::installed_version() const {
    std::string v = read_link(app_dir());
    struct stat st;
    return !v.empty() && ::stat((app_dir() + "/bin/max").c_str(), &st) == 0 ? v : std::string();
}

bool Installer::busy() const { return running_; }

Installer::Status Installer::status() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

void Installer::acknowledge() {
    std::lock_guard<std::mutex> lock(mu_);
    if (status_.phase == Phase::Done || status_.phase == Phase::Failed) status_.phase = Phase::Idle;
}

void Installer::set(Phase p, double progress) {
    std::lock_guard<std::mutex> lock(mu_);
    status_.phase = p;
    status_.progress = progress;
}

void Installer::start(bool install) {
    if (running_) return;
    if (worker_.joinable()) worker_.join();
    cancel_ = false;
    running_ = true;
    {
        std::lock_guard<std::mutex> lock(mu_);
        status_ = Status{};
        status_.phase = Phase::Checking;
    }
    worker_ = std::thread([this, install] {
        run(install);
        running_ = false;
    });
}

void Installer::cancel() { cancel_ = true; }

void Installer::remove_app() {
    if (running_) return;
    remove_tree(data_ + "/app");
    remove_tree(data_ + "/download");
}

void Installer::run(bool install) {
    auto fail = [this](const std::string& e) {
        std::lock_guard<std::mutex> lock(mu_);
        status_.phase = Phase::Failed;
        status_.error = e;
    };
    std::string index, error;
    if (!https_get(std::string(kRepository) + kIndex, index, error)) return fail(error);
    PackageInfo latest;
    if (!parse_packages(index, "max", "amd64", latest)) return fail("MAX is not in the repository index");
    {
        std::lock_guard<std::mutex> lock(mu_);
        status_.latest = latest;
        status_.total = latest.size;
    }
    if (!install || latest.version == installed_version()) return set(Phase::Done, 1);
    // Package file names come from the index: keep them to a plain path in the repository.
    if (latest.filename.find("..") != std::string::npos || latest.filename[0] == '/')
        return fail("unexpected file name in the repository index");

    std::string dl = data_ + "/download";
    ::mkdir(dl.c_str(), 0700);
    std::string deb = dl + "/max-" + latest.version + ".deb";
    set(Phase::Downloading);
    auto progress = [this](uint64_t done, uint64_t total) {
        std::lock_guard<std::mutex> lock(mu_);
        if (total) status_.total = total;
        status_.progress = status_.total ? double(done) / double(status_.total) : 0;
    };
    if (!https_download(kRepository + latest.filename, deb, progress, cancel_, error)) return fail(error);

    set(Phase::Verifying);
    std::string sum = sha256_file(deb, cancel_);
    if (cancel_) return fail("cancelled");
    if (sum != latest.sha256) {
        ::remove(deb.c_str());  // never install what does not match the index
        return fail("the download is damaged (checksum mismatch), try again");
    }

    set(Phase::Extracting);
    std::string apps = data_ + "/app";
    ::mkdir(apps.c_str(), 0755);
    std::string target = apps + "/" + latest.version, tmp = target + ".new";
    remove_tree(tmp);
    if (!extract_deb(deb, "usr/share/max/", tmp, [this](uint64_t done, uint64_t total) {
            std::lock_guard<std::mutex> lock(mu_);
            status_.progress = total ? double(done) / double(total) : 0;
        }, cancel_, error)) {
        remove_tree(tmp);
        return fail(error);
    }
    remove_tree(target);
    if (::rename(tmp.c_str(), target.c_str()) != 0) return fail("cannot install into " + target);
    // Switch atomically, then drop the old versions and the package.
    std::string link = apps + "/current.new";
    ::unlink(link.c_str());
    if (::symlink(latest.version.c_str(), link.c_str()) != 0 || ::rename(link.c_str(), app_dir().c_str()) != 0)
        return fail("cannot activate the new version");
    if (DIR* d = opendir(apps.c_str())) {
        std::vector<std::string> old;
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n != "." && n != ".." && n != "current" && n != latest.version) old.push_back(apps + "/" + n);
        }
        closedir(d);
        for (const auto& o : old) remove_tree(o);
    }
    remove_tree(dl);
    std::lock_guard<std::mutex> lock(mu_);
    status_.phase = Phase::Done;
    status_.progress = 1;
    status_.installed_now = true;
}

}  // namespace maxmod
