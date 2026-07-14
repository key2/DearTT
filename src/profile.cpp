#include "profile.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "event_server.hpp"  // findResource

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string sanitize(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            continue;
        out += c;
    }
    while (!out.empty() && (out.front() == ' ' || out.front() == '.'))
        out.erase(out.begin());
    while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();
    return out;
}

std::string stripAt(std::string s) {
    if (!s.empty() && s[0] == '@') s.erase(s.begin());
    return s;
}

}  // namespace

namespace profiles {

std::string root() {
    std::error_code ec;
    std::string r = findResource("profiles");
    if (r.empty()) {
#ifdef DEARTT_SOURCE_DIR
        r = std::string(DEARTT_SOURCE_DIR) + "/profiles";
#else
        r = "profiles";
#endif
    }
    fs::create_directories(r, ec);
    return r;
}

std::vector<std::string> list() {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root(), ec)) {
        if (!e.is_directory(ec)) continue;
        if (fs::exists(e.path() / "profile.json", ec))
            names.push_back(e.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool load(const std::string& name, Profile& out) {
    std::error_code ec;
    fs::path dir = fs::path(root()) / name;
    if (!fs::exists(dir / "profile.json", ec)) return false;
    out = Profile{};
    out.name = name;
    out.dir = dir.string();
    std::ifstream f(dir / "profile.json");
    if (f) {
        json j = json::parse(f, nullptr, false);
        if (!j.is_discarded()) out.stream = j.value("stream", "");
    }
    return true;
}

bool create(const std::string& name, const std::string& stream) {
    std::string clean = sanitize(name);
    if (clean.empty()) return false;
    std::error_code ec;
    fs::path dir = fs::path(root()) / clean;
    if (fs::exists(dir, ec)) return false;
    if (!fs::create_directories(dir, ec)) return false;
    json j;
    j["stream"] = stripAt(stream);
    std::ofstream f(dir / "profile.json");
    if (!f) return false;
    f << j.dump(2);
    return true;
}

bool setStream(const std::string& name, const std::string& stream) {
    std::error_code ec;
    fs::path dir = fs::path(root()) / name;
    if (!fs::exists(dir, ec)) return false;
    json j;
    j["stream"] = stripAt(stream);
    std::ofstream f(dir / "profile.json");
    if (!f) return false;
    f << j.dump(2);
    return true;
}

}  // namespace profiles
