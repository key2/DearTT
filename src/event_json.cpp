#include "event_json.hpp"

#include <chrono>
#include <memory>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>
#include <nlohmann/json.hpp>

#include "ttlive/events.hpp"

using nlohmann::json;

namespace {

std::string base64(const uint8_t* data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = data[i] << 16;
        if (i + 1 < len) v |= data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        out += i + 2 < len ? tbl[v & 63] : '=';
    }
    return out;
}

json userJson(const ttlive::User& u) {
    return json{{"id", u.id},
                {"unique_id", u.unique_id},
                {"nickname", u.nickname},
                {"verified", u.verified},
                {"avatar_url", u.avatar_url}};
}

bool hasUser(const ttlive::User& u) {
    return u.id != 0 || !u.unique_id.empty() || !u.nickname.empty();
}

json streamJson(const ttlive::StreamInfo& s) {
    json qualities = json::array();
    for (const auto& q : s.qualities)
        qualities.push_back(json{{"quality", q.quality},
                                 {"label", q.label},
                                 {"flv_url", q.flv_url},
                                 {"hls_url", q.hls_url}});
    return json{{"default_quality", s.default_quality},
                {"rtmp_pull_url", s.rtmp_pull_url},
                {"qualities", std::move(qualities)}};
}

// The fields ttlive surfaced for this event type (a convenient, stable
// subset; the full message is in "raw").
json dataJson(const ttlive::Event& e) {
    using ttlive::EventType;
    json d = json::object();
    if (hasUser(e.user)) d["user"] = userJson(e.user);
    switch (e.type) {
        case EventType::Connect:
            d["unique_id"] = e.unique_id;
            d["room_id"] = e.room_id;
            d["stream"] = streamJson(e.stream);
            break;
        case EventType::Comment:
            d["comment"] = e.comment;
            break;
        case EventType::Gift:
            d["gift_id"] = e.gift_id;
            d["gift_name"] = e.gift_name;
            d["repeat_count"] = e.repeat_count;
            d["gift_streaking"] = e.gift_streaking;
            d["diamond_count"] = e.diamond_count;
            d["gift_type"] = e.gift_type;  // 1 = streakable
            break;
        case EventType::Like:
            d["like_count"] = e.like_count;
            d["total_likes"] = e.total_likes;
            break;
        case EventType::Join:
        case EventType::Follow:
        case EventType::Share:
            d["member_count"] = e.member_count;
            break;
        case EventType::RoomUserSeq:
            d["viewer_count"] = e.viewer_count;
            break;
        case EventType::Control:
        case EventType::LiveEnd:
            d["control_action"] = e.control_action;
            break;
        default:
            break;
    }
    return d;
}

// Decode the raw Webcast protobuf generically: the message name maps to a
// generated type in the "tiktok" package, instantiated via reflection so
// every message in the schema (~650 types) works without listing them.
bool rawJson(const ttlive::Event& e, json& out) {
    namespace pb = google::protobuf;
    if (e.raw_payload.empty() || e.method.empty()) return false;

    const pb::Descriptor* desc =
        pb::DescriptorPool::generated_pool()->FindMessageTypeByName(
            "tiktok." + e.method);
    // Some methods arrive without the "Webcast" prefix (e.g. "RoomMessage"
    // -> tiktok.WebcastRoomMessage).
    if (!desc && e.method.rfind("Webcast", 0) != 0)
        desc = pb::DescriptorPool::generated_pool()->FindMessageTypeByName(
            "tiktok.Webcast" + e.method);
    if (!desc) return false;

    std::unique_ptr<pb::Message> msg(
        pb::MessageFactory::generated_factory()->GetPrototype(desc)->New());
    if (!msg->ParseFromArray(e.raw_payload.data(),
                             (int)e.raw_payload.size()))
        return false;

    pb::util::JsonPrintOptions opts;
    opts.preserve_proto_field_names = true;  // snake_case, as in the .proto

    std::string text;
    if (!pb::util::MessageToJsonString(*msg, &text, opts).ok()) return false;

    json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) return false;
    out = std::move(parsed);
    return true;
}

}  // namespace

std::string eventToJson(const ttlive::Event& e) {
    json j;
    j["type"] = ttlive::to_string(e.type);
    j["method"] = e.method;
    j["ts_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
    j["data"] = dataJson(e);

    json raw;
    if (rawJson(e, raw))
        j["raw"] = std::move(raw);
    else if (!e.raw_payload.empty())
        j["raw_base64"] = base64(e.raw_payload.data(), e.raw_payload.size());

    // Comments and nicknames can contain lone surrogates / invalid UTF-8;
    // replace instead of throwing.
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}
