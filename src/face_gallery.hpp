#pragma once

// FaceGallery — the roster of people for one profile: a managed list of names,
// each with AT MOST ONE face embedding ("landmark") plus the quality (face
// size) at which it was captured. Stored in <profileDir>/identities.json.
//
// Design goals (per product requirements):
//   * The list of people is explicitly managed (add / delete / reset).
//   * There are never more embeddings than people — one per person.
//   * A person may exist with no embedding yet (added but not seen).
//   * A better capture (bigger/closer face) replaces the stored embedding.

#include <mutex>
#include <string>
#include <vector>

struct Identity {
    std::string name;
    std::vector<float> embedding;  // empty = no face captured for this person
    float quality = 0.0f;          // face area (px²) when captured; for upgrade
    std::string avatar;            // local path to a DISPLAY-ONLY picture
};

struct PersonInfo {
    std::string name;
    bool hasFace = false;
    float quality = 0.0f;
    std::string avatar;            // display-only picture path ("" if none)
};

class FaceGallery {
public:
    bool load(const std::string& dir);   // reads dir/identities.json
    bool save() const;                   // writes dir/identities.json
    void setDir(const std::string& dir) { dir_ = dir; }
    std::string dir() const { return dir_; }

    // --- roster management (one entry per person) ----------------------
    void addPerson(const std::string& name);     // no-op if already present
    void removePerson(const std::string& name);
    void resetPerson(const std::string& name);   // clear that person's face
    /// Set a person's display picture path (never affects recognition).
    void setAvatar(const std::string& name, const std::string& path);
    /// Set/replace a person's embedding unconditionally (manual assignment).
    void assign(const std::string& name, const std::vector<float>& emb,
                float quality);
    /// Replace only if `quality` beats the stored one by a margin. Returns
    /// true if it replaced (so the caller can persist).
    bool maybeImprove(const std::string& name, const std::vector<float>& emb,
                      float quality);

    /// Closest match among people that have an embedding. ("", 0) if none
    /// above `threshold`.
    std::pair<std::string, float> identify(const std::vector<float>& emb,
                                           float threshold = 0.35f) const;

    std::vector<PersonInfo> roster() const;
    size_t size() const;
    bool empty() const { return size() == 0; }

private:
    mutable std::mutex mutex_;
    std::string dir_;
    std::vector<Identity> identities_;
};
