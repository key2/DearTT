#include "face_gallery.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

bool FaceGallery::load(const std::string& dir) {
    std::lock_guard<std::mutex> lk(mutex_);
    dir_ = dir;
    identities_.clear();

    fs::path path = fs::path(dir) / "identities.json";
    std::error_code ec;
    if (!fs::exists(path, ec)) return true;  // empty roster, not an error

    std::ifstream f(path);
    if (!f) return false;
    json j = json::parse(f, nullptr, false);
    if (j.is_discarded()) return false;

    for (const auto& e : j) {
        Identity id;
        id.name = e.value("name", "");
        if (id.name.empty()) continue;
        id.quality = e.value("quality", 0.0f);
        id.avatar = e.value("avatar", "");
        if (e.contains("embedding") && e["embedding"].is_array())
            id.embedding = e["embedding"].get<std::vector<float>>();
        identities_.push_back(std::move(id));
    }
    return true;
}

bool FaceGallery::save() const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (dir_.empty()) return false;
    std::error_code ec;
    fs::create_directories(dir_, ec);

    json j = json::array();
    for (const auto& id : identities_) {
        json e;
        e["name"] = id.name;
        if (!id.embedding.empty()) {
            e["embedding"] = id.embedding;
            e["quality"] = id.quality;
        }
        if (!id.avatar.empty()) e["avatar"] = id.avatar;
        j.push_back(std::move(e));
    }
    std::ofstream f(fs::path(dir_) / "identities.json");
    if (!f) return false;
    f << j.dump(2);
    return true;
}

void FaceGallery::addPerson(const std::string& name) {
    if (name.empty()) return;
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& id : identities_)
        if (id.name == name) return;
    identities_.push_back(Identity{name, {}, 0.0f});
}

void FaceGallery::removePerson(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    identities_.erase(
        std::remove_if(identities_.begin(), identities_.end(),
                       [&](const Identity& id) { return id.name == name; }),
        identities_.end());
}

void FaceGallery::resetPerson(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& id : identities_)
        if (id.name == name) {
            id.embedding.clear();
            id.quality = 0.0f;
        }
}

void FaceGallery::setAvatar(const std::string& name, const std::string& path) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& id : identities_)
        if (id.name == name) { id.avatar = path; return; }
    // Person may not exist yet (avatar set before add) — create a stub.
    Identity id;
    id.name = name;
    id.avatar = path;
    identities_.push_back(std::move(id));
}

void FaceGallery::assign(const std::string& name,
                         const std::vector<float>& emb, float quality) {
    if (name.empty() || emb.empty()) return;
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& id : identities_)
        if (id.name == name) {
            id.embedding = emb;
            id.quality = quality;
            return;
        }
    identities_.push_back(Identity{name, emb, quality});
}

bool FaceGallery::maybeImprove(const std::string& name,
                               const std::vector<float>& emb, float quality) {
    if (name.empty() || emb.empty()) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& id : identities_) {
        if (id.name != name) continue;
        // Replace only when the new face is meaningfully bigger/closer.
        if (id.embedding.empty() || quality > id.quality * 1.15f) {
            id.embedding = emb;
            id.quality = quality;
            return true;
        }
        return false;
    }
    return false;
}

std::pair<std::string, float> FaceGallery::identify(
    const std::vector<float>& emb, float threshold) const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::string bestName;
    float bestSim = -1.0f;
    for (const auto& id : identities_) {
        if (id.embedding.size() != emb.size()) continue;
        float dot = 0.0f;  // embeddings are L2-normalized -> cosine similarity
        for (size_t i = 0; i < emb.size(); i++) dot += emb[i] * id.embedding[i];
        if (dot > bestSim) { bestSim = dot; bestName = id.name; }
    }
    if (bestSim >= threshold) return {bestName, bestSim};
    return {"", 0.0f};
}

std::vector<PersonInfo> FaceGallery::roster() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<PersonInfo> out;
    out.reserve(identities_.size());
    for (const auto& id : identities_)
        out.push_back({id.name, !id.embedding.empty(), id.quality, id.avatar});
    return out;
}

size_t FaceGallery::size() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return identities_.size();
}
