#pragma once

// Profile — a named workspace pairing a TikTok stream with the people expected
// on it. The roster of people (and their face landmarks) lives in the profile
// folder's identities.json, managed by FaceTracker; the profile itself just
// records which stream to connect to.
//
// On disk:
//   profiles/
//     <ProfileName>/
//       profile.json        { "stream": "username" }
//       identities.json     roster + one face embedding per person

#include <string>
#include <vector>

struct Profile {
    std::string name;    // == folder name
    std::string dir;     // absolute path to the profile folder
    std::string stream;  // TikTok @username (no '@')
};

namespace profiles {

/// Root directory holding all profiles (next to the exe, CWD, or source tree).
std::string root();

/// Names of all profiles found under root().
std::vector<std::string> list();

/// Load a profile by name. Returns false if it doesn't exist.
bool load(const std::string& name, Profile& out);

/// Create a new profile folder with profile.json. False if name exists/invalid.
bool create(const std::string& name, const std::string& stream);

/// Persist the stream field of an existing profile.
bool setStream(const std::string& name, const std::string& stream);

}  // namespace profiles
