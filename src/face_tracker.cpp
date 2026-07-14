#include "face_tracker.hpp"
#include "face_embedder.hpp"
#include "face_gallery.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>

namespace {

float iouBoxes(const FaceBox& a, const FaceBox& b) {
    float x0 = std::max(a.x, b.x);
    float y0 = std::max(a.y, b.y);
    float x1 = std::min(a.x + a.w, b.x + b.w);
    float y1 = std::min(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) return 0.0f;
    float inter = (x1 - x0) * (y1 - y0);
    return inter / (a.w * a.h + b.w * b.h - inter);
}

float centroidDistNorm(const FaceBox& a, const FaceBox& b) {
    float acx = a.x + a.w * 0.5f, acy = a.y + a.h * 0.5f;
    float bcx = b.x + b.w * 0.5f, bcy = b.y + b.h * 0.5f;
    float dx = acx - bcx, dy = acy - bcy;
    float dist = std::sqrt(dx * dx + dy * dy);
    float scale = 0.5f * (a.w + b.w) * 0.5f + 0.5f * (a.h + b.h) * 0.5f;
    return scale > 1e-3f ? dist / scale : 1e9f;
}

double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

bool FaceTracker::start(const std::string& modelsDir) {
    stop();
    quit_.store(false);
    {
        std::lock_guard<std::mutex> lk(resultMutex_);
        status_ = "loading models...";
    }
    thread_ = std::thread(&FaceTracker::threadMain, this, modelsDir);
    return true;
}

void FaceTracker::stop() {
    quit_.store(true);
    frameCv_.notify_all();
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

void FaceTracker::submitFrame(const uint8_t* rgba, int w, int h) {
    std::lock_guard<std::mutex> lk(frameMutex_);
    if (frameReady_) return;  // analysis thread still busy with prev frame
    frameData_.resize((size_t)w * h * 4);
    std::memcpy(frameData_.data(), rgba, frameData_.size());
    frameW_ = w;
    frameH_ = h;
    frameReady_ = true;
    frameCv_.notify_one();
}

std::vector<TrackedFace> FaceTracker::faces() const {
    std::lock_guard<std::mutex> lk(resultMutex_);
    return faces_;
}

std::vector<PersonInfo> FaceTracker::roster() const {
    std::lock_guard<std::mutex> lk(resultMutex_);
    return roster_;
}

void FaceTracker::selectProfile(const std::string& profileDir) {
    {
        std::lock_guard<std::mutex> lk(frameMutex_);
        pendingProfileDir_ = profileDir;
    }
    frameCv_.notify_one();
}

void FaceTracker::addPerson(const std::string& name) {
    { std::lock_guard<std::mutex> lk(frameMutex_);
      pendingCmds_.push_back({Cmd::Add, name, ""}); }
    frameCv_.notify_one();
}
void FaceTracker::removePerson(const std::string& name) {
    { std::lock_guard<std::mutex> lk(frameMutex_);
      pendingCmds_.push_back({Cmd::Remove, name, ""}); }
    frameCv_.notify_one();
}
void FaceTracker::resetPerson(const std::string& name) {
    { std::lock_guard<std::mutex> lk(frameMutex_);
      pendingCmds_.push_back({Cmd::Reset, name, ""}); }
    frameCv_.notify_one();
}
void FaceTracker::assignEmbedding(const std::string& person,
                                  const std::vector<float>& emb,
                                  float quality) {
    if (emb.empty()) return;
    { std::lock_guard<std::mutex> lk(frameMutex_);
      pendingEmbAssign_.push_back({person, emb, quality}); }
    frameCv_.notify_one();
}
void FaceTracker::setAvatar(const std::string& person, const std::string& path) {
    { std::lock_guard<std::mutex> lk(frameMutex_);
      pendingCmds_.push_back({Cmd::SetAvatar, person, path}); }
    frameCv_.notify_one();
}

void FaceTracker::setIdentityWindow(float seconds) {
    if (seconds < 1.0f) seconds = 1.0f;
    if (seconds > 10.0f) seconds = 10.0f;
    windowSec_.store(seconds);
}

std::string FaceTracker::status() const {
    std::lock_guard<std::mutex> lk(resultMutex_);
    return status_;
}

void FaceTracker::threadMain(std::string modelsDir) {
    FaceDetector detector;
    FaceEmbedder embedder;
    FaceGallery gallery;

    try {
        if (!detector.load(modelsDir + "/det_500m.onnx")) {
            std::lock_guard<std::mutex> lk(resultMutex_);
            status_ = "failed to load detection model";
            return;
        }
        if (!embedder.load(modelsDir + "/w600k_mbf.onnx")) {
            std::lock_guard<std::mutex> lk(resultMutex_);
            status_ = "failed to load recognition model";
            return;
        }
    } catch (...) {
        std::lock_guard<std::mutex> lk(resultMutex_);
        status_ = "ONNX Runtime unavailable (CPU may lack AVX2 or Windows too old)";
        return;
    }

    {
        std::lock_guard<std::mutex> lk(resultMutex_);
        status_ = "ready (no profile)";
    }
    running_.store(true);

    auto publishRoster = [&] {
        auto r = gallery.roster();
        std::lock_guard<std::mutex> lk(resultMutex_);
        roster_ = std::move(r);
    };

    std::vector<Track> tracks;

    // --- tuning ---------------------------------------------------------
    constexpr float kIouMatch = 0.30f;
    constexpr float kCentroidMatch = 1.20f;
    constexpr float kBoxEma = 0.5f;
    constexpr int kEmbedEvery = 5;
    constexpr int kMinDetections = 3;
    constexpr double kCoastShow = 1.0;
    constexpr float kMinVote = 0.50f;
    constexpr float kImproveSim = 0.55f;  // only upgrade on a strong match
    constexpr float kMatchThresh = 0.45f; // min cosine to accept a person

    while (!quit_.load()) {
        std::string profileReq;
        std::vector<Cmd> cmds;
        std::vector<EmbAssign> embAssigns;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        bool haveFrame = false;
        {
            std::unique_lock<std::mutex> lk(frameMutex_);
            frameCv_.wait(lk, [&] {
                return quit_.load() || frameReady_ ||
                       !pendingProfileDir_.empty() || !pendingCmds_.empty() ||
                       !pendingEmbAssign_.empty();
            });
            if (quit_.load()) break;
            if (!pendingProfileDir_.empty()) {
                profileReq = std::move(pendingProfileDir_);
                pendingProfileDir_.clear();
            }
            if (!pendingCmds_.empty()) cmds.swap(pendingCmds_);
            if (!pendingEmbAssign_.empty()) embAssigns.swap(pendingEmbAssign_);
            if (frameReady_) {
                rgba = std::move(frameData_);
                w = frameW_;
                h = frameH_;
                frameReady_ = false;
                haveFrame = true;
            }
        }

        // Profile switch: load roster + embeddings, drop tracks.
        if (!profileReq.empty()) {
            gallery.load(profileReq);
            gallery.setDir(profileReq);
            tracks.clear();
            publishRoster();
            std::lock_guard<std::mutex> lk(resultMutex_);
            status_ = "profile loaded (" + std::to_string(gallery.size()) +
                      " people)";
        }

        // Roster commands.
        bool rosterChanged = false;
        for (const auto& c : cmds) {
            switch (c.type) {
                case Cmd::Add: gallery.addPerson(c.name); rosterChanged = true;
                    break;
                case Cmd::Remove: gallery.removePerson(c.name);
                    rosterChanged = true; break;
                case Cmd::Reset: gallery.resetPerson(c.name);
                    rosterChanged = true; break;
                case Cmd::SetAvatar:
                    gallery.setAvatar(c.name, c.arg);
                    rosterChanged = true;
                    break;
            }
        }
        // Embedding assignments snapshotted at click time (target is fixed;
        // immune to any track changes while the popup was open).
        for (const auto& a : embAssigns) {
            gallery.assign(a.name, a.emb, a.quality);
            rosterChanged = true;
        }
        if (rosterChanged) { gallery.save(); publishRoster(); }

        if (!haveFrame || rgba.empty() || w <= 0 || h <= 0) continue;

        const double t = nowSec();
        const float window = windowSec_.load();

        // 1) Detect faces.
        auto boxes = detector.detect(rgba.data(), w, h, 0.5f, 0.4f);

        // 2) Greedy match detections to existing tracks (IOU, then centroid).
        std::vector<int> detTrack(boxes.size(), -1);
        std::vector<bool> trackUsed(tracks.size(), false);
        for (size_t d = 0; d < boxes.size(); d++) {
            int best = -1;
            float bestIou = kIouMatch;
            for (size_t ti = 0; ti < tracks.size(); ti++) {
                if (trackUsed[ti]) continue;
                float iou = iouBoxes(boxes[d], tracks[ti].box);
                if (iou > bestIou) { bestIou = iou; best = (int)ti; }
            }
            if (best < 0) {
                float bestDist = kCentroidMatch;
                for (size_t ti = 0; ti < tracks.size(); ti++) {
                    if (trackUsed[ti]) continue;
                    float dist = centroidDistNorm(boxes[d], tracks[ti].box);
                    if (dist < bestDist) { bestDist = dist; best = (int)ti; }
                }
            }
            if (best >= 0) { detTrack[d] = best; trackUsed[best] = true; }
        }

        // 3) Update matched tracks / create new ones; embed (throttled).
        bool improved = false;
        for (size_t d = 0; d < boxes.size(); d++) {
            int ti = detTrack[d];
            if (ti < 0) {
                Track tr;
                tr.id = nextTrackId_++;
                tr.box = boxes[d];
                tr.firstSeen = tr.lastSeen = t;
                tr.sinceEmbed = kEmbedEvery;
                tracks.push_back(std::move(tr));
                ti = (int)tracks.size() - 1;
                detTrack[d] = ti;
                trackUsed.push_back(true);
            }
            Track& tr = tracks[ti];
            tr.box.x = kBoxEma * boxes[d].x + (1 - kBoxEma) * tr.box.x;
            tr.box.y = kBoxEma * boxes[d].y + (1 - kBoxEma) * tr.box.y;
            tr.box.w = kBoxEma * boxes[d].w + (1 - kBoxEma) * tr.box.w;
            tr.box.h = kBoxEma * boxes[d].h + (1 - kBoxEma) * tr.box.h;
            std::memcpy(tr.box.landmarks, boxes[d].landmarks,
                        sizeof(tr.box.landmarks));
            tr.box.score = boxes[d].score;
            tr.lastSeen = t;
            tr.seenFrames++;
            tr.sinceEmbed++;

            if (tr.sinceEmbed >= kEmbedEvery) {
                tr.sinceEmbed = 0;
                auto emb = embedder.embed(rgba.data(), w, h, boxes[d].landmarks);
                if (!emb.empty()) {
                    float area = boxes[d].w * boxes[d].h;
                    tr.lastEmb = emb;      // kept for click-to-assign
                    tr.lastArea = area;
                    auto [name, sim] = gallery.identify(emb, kMatchThresh);
                    tr.history.push_back({t, name, sim});
                    // Auto-upgrade: a closer/bigger face of a confidently
                    // recognized person replaces the stored landmark.
                    if (!name.empty() && sim >= kImproveSim &&
                        gallery.maybeImprove(name, emb, area))
                        improved = true;
                }
            }
        }
        if (improved) { gallery.save(); publishRoster(); }

        // 4) Expire dead tracks, prune history, vote, emit.
        // Keep a track alive longer than the (possibly short) vote window so
        // a brief detector miss / head-turn doesn't spawn a fresh track (and
        // a fresh number) when the same person reappears.
        double keepAlive = std::max((double)window, 4.0);
        std::vector<TrackedFace> out;
        for (auto it = tracks.begin(); it != tracks.end();) {
            Track& tr = *it;
            if (t - tr.lastSeen > keepAlive) { it = tracks.erase(it); continue; }
            while (!tr.history.empty() && t - tr.history.front().t > window)
                tr.history.pop_front();

            std::map<std::string, int> counts;
            std::map<std::string, float> bestSim;
            int named = 0;
            for (const auto& s : tr.history) {
                if (s.name.empty()) continue;
                counts[s.name]++;
                bestSim[s.name] = std::max(bestSim[s.name], s.sim);
                named++;
            }
            tr.voteName.clear();
            tr.voteConf = 0.0f;
            tr.voteSim = 0.0f;
            if (named > 0) {
                std::string top;
                int topCount = 0;
                for (const auto& [n, c] : counts)
                    if (c > topCount) { topCount = c; top = n; }
                float conf = (float)topCount / (float)named;
                if (conf >= kMinVote) {
                    tr.voteName = top;
                    tr.voteConf = conf;
                    tr.voteSim = bestSim[top];
                }
            }

            bool coasting = (t - tr.lastSeen) > 0.20;
            bool withinCoast = (t - tr.lastSeen) <= kCoastShow;
            if (tr.seenFrames >= kMinDetections && withinCoast) {
                TrackedFace tf;
                tf.box = tr.box;
                tf.trackId = tr.id;
                tf.name = tr.voteName;
                tf.similarity = tr.voteSim;
                tf.voteConfidence = tr.voteConf;
                tf.coasting = coasting;
                tf.embedding = tr.lastEmb;  // snapshot source for assignment
                out.push_back(std::move(tf));
            }
            ++it;
        }

        // Mutual exclusivity: a known person can label at most one face per
        // frame — the one that matches them best. Any other face that also
        // claimed that name is demoted to unknown (shown as "?"), so two
        // different people can never both read as the same person.
        {
            std::map<std::string, int> bestForName;  // name -> index in out
            for (int i = 0; i < (int)out.size(); i++) {
                if (out[i].name.empty()) continue;
                auto it = bestForName.find(out[i].name);
                if (it == bestForName.end() ||
                    out[i].similarity > out[it->second].similarity)
                    bestForName[out[i].name] = i;
            }
            for (int i = 0; i < (int)out.size(); i++) {
                if (out[i].name.empty()) continue;
                if (bestForName[out[i].name] != i) {
                    out[i].name.clear();
                    out[i].voteConfidence = 0.0f;
                    out[i].similarity = 0.0f;
                }
            }
        }

        std::sort(out.begin(), out.end(),
                  [](const TrackedFace& a, const TrackedFace& b) {
                      return a.trackId < b.trackId;
                  });

        {
            std::lock_guard<std::mutex> lk(resultMutex_);
            faces_ = std::move(out);
        }
    }
}
