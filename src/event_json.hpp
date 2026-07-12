#pragma once

// eventToJson — serialize a ttlive::Event to a JSON envelope:
//
//   {
//     "type":   "Comment",              // ttlive::EventType name
//     "method": "WebcastChatMessage",   // underlying Webcast message
//     "ts_ms":  1720795123456,          // local receive time (unix ms)
//     "data":   { ... },                // fields ttlive surfaced for the type
//     "raw":    { ... }                 // FULL protobuf decoded to JSON, or
//     "raw_base64": "..."               // the bytes if it can't be decoded
//   }
//
// "raw" is produced generically via protobuf reflection from the message
// name, so every Webcast message — including ones ttlive reports as
// Unknown — is forwarded with 100% of its content.

#include <string>

namespace ttlive {
struct Event;
}

std::string eventToJson(const ttlive::Event& e);
