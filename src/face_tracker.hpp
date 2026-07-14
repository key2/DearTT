#pragma once

// FaceTracker — runs detection + recognition on a background thread and
// maintains persistent face tracks with temporal identity voting.
//
// Key behaviours:
//   * Stable track numbers  — each face keeps its #N as long as it stays in
//     roughly the same place (spatial matching), instead of being renumbered
//     by size every frame.
//   * Temporal identity vote — the shown name is the majority over a sliding
//     window (configurable 1-10 s), so a single bad frame can't flip it.
//   * Coasting — a track survives brief detection gaps (head turned away,
//     motion blur) for a grace period, keeping its position and identity.
//   * False-positive suppression — a track is only shown once it has been
//     seen in enough of the recent window.
//
// Usage:
//   FaceTracker tracker;
//   tracker.start("models", "gallery");
//   tracker.submitFrame(rgba, w, h);   // after takeFrame()
//   auto faces = tracker.faces();      // read latest results (UI thread)
//   tracker.stop();

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "face_detector.hpp"
#include "face_gallery.hpp"  // PersonInfo

struct TrackedFace {
    FaceBox box;
    std::string name;        // "" if unknown / not yet confident
    float similarity;        // best cosine similarity backing the vote
    float voteConfidence;    // fraction of the window agreeing on `name` [0,1]
    int trackId;             // stable id, shown as the #N badge
    bool coasting;           // true when shown from memory (no detection now)
    std::vector<float> embedding;  // this face's current embedding (for
                                   // click-to-assign: snapshot it at click)
};

class FaceTracker {
public:
    FaceTracker() = default;
    ~FaceTracker() { stop(); }

    FaceTracker(const FaceTracker&) = delete;
    FaceTracker& operator=(const FaceTracker&) = delete;

    /// Load models and start the analysis thread. No faces are known until a
    /// profile is selected. modelsDir contains det_500m.onnx + w600k_mbf.onnx.
    bool start(const std::string& modelsDir);
    void stop();

    /// Switch to a profile: load its roster (people + embeddings) from
    /// profileDir/identities.json and use it for live recognition. Clears
    /// current tracks. Runs on the analysis thread.
    void selectProfile(const std::string& profileDir);

    // --- roster management (applied on the analysis thread, then saved) ---
    void addPerson(const std::string& name);
    void removePerson(const std::string& name);
    void resetPerson(const std::string& name);      // clear that person's face
    /// Assign a *snapshotted* embedding (captured from a face at click time)
    /// to `person`. Independent of live track state, so nothing can change
    /// the target between the click and confirming the name.
    void assignEmbedding(const std::string& person,
                         const std::vector<float>& emb, float quality);
    /// Set a person's display picture (cosmetic; not a recognition reference).
    void setAvatar(const std::string& person, const std::string& path);

    /// Current roster snapshot (names + whether each has a captured face).
    std::vector<PersonInfo> roster() const;

    bool running() const { return running_.load(); }

    /// Submit the latest decoded frame for analysis (non-blocking; if the
    /// thread is still busy, this frame is skipped). The RGBA buffer is
    /// copied internally.
    void submitFrame(const uint8_t* rgba, int w, int h);

    /// Get the latest tracked faces (thread-safe copy).
    std::vector<TrackedFace> faces() const;

    /// Sliding window (seconds) used for the temporal identity vote and for
    /// how long a lost track coasts before being dropped. Clamped [1, 10].
    void setIdentityWindow(float seconds);
    float identityWindow() const { return windowSec_.load(); }

    /// Status message (model loading / errors).
    std::string status() const;

private:
    void threadMain(std::string modelsDir);

    // One recognition observation for the voting window.
    struct Sample {
        double t;
        std::string name;   // "" = detected but unknown
        float sim;
    };

    // A persistent face track across frames.
    struct Track {
        int id = 0;
        FaceBox box{};          // EMA-smoothed position
        double firstSeen = 0.0;
        double lastSeen = 0.0;  // last frame with an actual detection
        int seenFrames = 0;     // total frames this track was detected
        std::deque<Sample> history;
        int sinceEmbed = 0;     // frames since last embedding (throttling)
        std::vector<float> lastEmb;  // most recent embedding (for assignment)
        float lastArea = 0.0f;       // face area when lastEmb was computed
        // Cached majority vote (recomputed each frame from history).
        std::string voteName;
        float voteConf = 0.0f;
        float voteSim = 0.0f;
    };

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> quit_{false};
    std::atomic<float> windowSec_{4.0f};

    // Frame submission.
    mutable std::mutex frameMutex_;
    std::condition_variable frameCv_;
    std::vector<uint8_t> frameData_;
    int frameW_ = 0, frameH_ = 0;
    bool frameReady_ = false;

    // Results.
    mutable std::mutex resultMutex_;
    std::vector<TrackedFace> faces_;
    std::string status_ = "not started";
    std::vector<PersonInfo> roster_;   // snapshot for the UI

    // Requests processed on the analysis thread (guarded by frameMutex_,
    // woken via frameCv_). Kept beside the frame so one CV covers both.
    struct Cmd {
        enum Type { Add, Remove, Reset, SetAvatar } type;
        std::string name;
        std::string arg;   // avatar path for SetAvatar
    };
    struct EmbAssign {
        std::string name;
        std::vector<float> emb;
        float quality = 0.0f;
    };
    std::string pendingProfileDir_;    // non-empty = switch
    std::vector<Cmd> pendingCmds_;
    std::vector<EmbAssign> pendingEmbAssign_;

    int nextTrackId_ = 1;
};
