#include "didi/gdextension/editor_hook.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/runtime/animation_requests.hpp"

#include <cstdio>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) \
    if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) \
    if (!((a) == (b))) throw std::runtime_error("Assertion failed: " #a " == " #b);

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using didi::runtime::AnimationInfo;
using didi::runtime::AnimationTrackInfo;
using didi::runtime::buildAnimationCatalog;
using didi::runtime::parseAnimListRequest;
using didi::runtime::parseAnimPlayRequest;

void test_registry_advertises_list_read_and_play_mutation() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* list = registry.getTool("anim_list_tracks");
    const auto* play = registry.getTool("anim_play_track");
    ASSERT_TRUE(list && play && list->capability.implemented && play->capability.implemented);
    ASSERT_EQ(list->toJson()["_meta"]["didi"]["executionModes"], json::array({"live"}));
    ASSERT_EQ(play->toJson()["_meta"]["didi"]["executionModes"], json::array({"live"}));
    ASSERT_TRUE(!list->inputSchema["properties"].contains("dry_run"));
    ASSERT_TRUE(play->inputSchema["properties"].contains("dry_run"));
    ASSERT_TRUE(!play->inputSchema["properties"].contains("confirmation_token"));
    const auto result = registry.callTool("anim_list_tracks", {{"animation_player_path", "/root/Player"}});
    ASSERT_TRUE(result.isError);
    ASSERT_TRUE(result.content[0].text.find("no trustworthy execution path") == std::string::npos);
}

void test_requests_parse_and_reject() {
    ASSERT_TRUE(parseAnimListRequest({{"animation_player_path", "/root/Player"}}).isOk());
    for (const auto& bad : {json::object(), json{{"animation_player_path", ""}},
                            json{{"animation_player_path", std::string(1025, 'p')}},
                            json{{"animation_player_path", "/root/Player"}, {"extra", 1}},
                            json{{"animation_player_path", 7}}}) {
        auto parsed = parseAnimListRequest(bad);
        ASSERT_TRUE(parsed.isErr());
        ASSERT_EQ(parsed.error().code, 400);
    }

    auto play = parseAnimPlayRequest({{"animation_player_path", "/root/Player"}, {"animation_name", "walk"}});
    ASSERT_TRUE(play.isOk());
    ASSERT_EQ(play.value().custom_speed, 1.0);
    ASSERT_TRUE(!play.value().from_end);
    auto reverse = parseAnimPlayRequest({{"animation_player_path", "/root/Player"}, {"animation_name", "walk"},
                                         {"custom_speed", -2.5}, {"from_end", true}});
    ASSERT_TRUE(reverse.isOk());
    ASSERT_EQ(reverse.value().custom_speed, -2.5);
    const double inf = std::numeric_limits<double>::infinity();
    const json base = {{"animation_player_path", "/root/Player"}, {"animation_name", "walk"}};
    auto with = [&base](const char* key, json value) { json copy = base; copy[key] = std::move(value); return copy; };
    for (const auto& bad : {json{{"animation_player_path", "/root/Player"}},
                            with("animation_name", ""), with("animation_name", std::string(257, 'w')),
                            with("custom_speed", 0), with("custom_speed", 16.5), with("custom_speed", -17),
                            with("custom_speed", inf), with("custom_speed", "fast"),
                            // Negative speed without from_end would play nothing from time zero.
                            with("custom_speed", -1), with("from_end", "yes"), with("blend", 0.5)}) {
        auto parsed = parseAnimPlayRequest(bad);
        ASSERT_TRUE(parsed.isErr());
        ASSERT_EQ(parsed.error().code, 400);
    }
}

AnimationInfo animation(const std::string& name, size_t tracks, size_t keys) {
    AnimationInfo info;
    info.name = name;
    info.length = 1.0;
    info.loop_mode_id = 1;
    for (size_t track = 0; track < tracks; ++track) {
        AnimationTrackInfo item;
        item.index = static_cast<int64_t>(track);
        item.type_id = static_cast<int64_t>(track % 10);
        item.path = "Target:position";
        for (size_t key = 0; key < keys; ++key) item.key_times.push_back(static_cast<double>(key) * 0.5);
        info.tracks.push_back(std::move(item));
    }
    return info;
}

void test_catalog_sorts_names_and_maps_enums() {
    auto catalog = buildAnimationCatalog({animation("walk", 1, 2), animation("Idle", 2, 1), animation("attack", 0, 0)});
    ASSERT_EQ(catalog["truncated"], false);
    ASSERT_TRUE(catalog["truncated_at"].is_null());
    const auto& animations = catalog["animations"];
    ASSERT_EQ(animations.size(), 3u);
    // Byte order, so the capital sorts first.
    ASSERT_EQ(animations[0]["name"], "Idle");
    ASSERT_EQ(animations[1]["name"], "attack");
    ASSERT_EQ(animations[2]["name"], "walk");
    ASSERT_EQ(animations[2]["loop_mode_name"], "linear");
    ASSERT_EQ(animations[2]["loop_mode_id"], 1);
    ASSERT_EQ(animations[2]["truncated"], false);
    const auto& track = animations[2]["tracks"][0];
    ASSERT_EQ(track["index"], 0);
    ASSERT_EQ(track["type_name"], "value");
    ASSERT_EQ(track["path"], "Target:position");
    ASSERT_EQ(track["key_times"], json::array({0.0, 0.5}));
    ASSERT_EQ(track["truncated"], false);
    ASSERT_EQ(animations[0]["tracks"][1]["type_name"], "position_3d");
    ASSERT_EQ(track.size(), 6u);
    ASSERT_EQ(animations[2].size(), 6u);
    ASSERT_EQ(didi::runtime::animationTrackTypeName(8), std::string("animation"));
    ASSERT_EQ(didi::runtime::animationTrackTypeName(9), std::string("unknown"));
    ASSERT_EQ(didi::runtime::animationLoopModeName(2), std::string("pingpong"));
    ASSERT_EQ(didi::runtime::animationLoopModeName(3), std::string("unknown"));
}

void test_catalog_applies_count_caps_with_a_cursor() {
    std::vector<AnimationInfo> many;
    for (int index = 0; index < 129; ++index) {
        char name[16];
        std::snprintf(name, sizeof(name), "anim_%03d", index);
        many.push_back(animation(name, 1, 1));
    }
    auto catalog = buildAnimationCatalog(many);
    ASSERT_EQ(catalog["animations"].size(), 128u);
    ASSERT_EQ(catalog["truncated"], true);
    ASSERT_EQ(catalog["truncated_at"]["animation_index"], 128);
    ASSERT_EQ(catalog["truncated_at"]["reason"], "count");

    auto wide = buildAnimationCatalog({animation("wide", 129, 257)});
    const auto& record = wide["animations"][0];
    ASSERT_EQ(record["tracks"].size(), 128u);
    ASSERT_EQ(record["truncated"], true);
    ASSERT_EQ(record["tracks"][0]["key_times"].size(), 256u);
    ASSERT_EQ(record["tracks"][0]["truncated"], true);
    // Per-record cuts do not set the catalog cursor; the catalog itself is whole.
    ASSERT_EQ(wide["truncated"], false);
}

void test_catalog_stops_before_a_record_at_the_byte_budget() {
    // Each animation here is about 12 KiB of key times, so the budget lands
    // mid-list. The catalog must stop before the record that would cross it.
    std::vector<AnimationInfo> heavy;
    for (int index = 0; index < 40; ++index) {
        heavy.push_back(animation("heavy_" + std::to_string(index), 6, 256));
    }
    auto catalog = buildAnimationCatalog(heavy);
    ASSERT_EQ(catalog["truncated"], true);
    ASSERT_EQ(catalog["truncated_at"]["reason"], "bytes");
    ASSERT_TRUE(catalog["animations"].size() < 40u);
    ASSERT_TRUE(catalog["animations"].size() > 0u);
    ASSERT_TRUE(catalog.dump().size() <= 256u * 1024u);
    ASSERT_EQ(catalog["truncated_at"]["animation_index"], catalog["animations"].size());
}

void test_hook_policy_for_list_and_play() {
    auto& hook = didi::godot::EditorHook::instance();
    hook.cancelPendingCommands("test reset");
    // The play is game-only and is refused before the bridge in an editor session.
    didi::godot::EditorHookTestAccess::setSessionKind(hook, didi::runtime::SessionKind::editor);
    auto refused = didi::godot::EditorHookTestAccess::executeOnMainThread(
        hook, "anim.playTrack", {{"animation_player_path", "/root/Player"}, {"animation_name", "walk"}});
    ASSERT_EQ(refused["error"]["code"], 409);
    ASSERT_EQ(refused["error"]["message"], "session_kind_rejected");
    // The list reaches the bridge in a game session; without an engine that
    // is a not-ready error rather than a policy refusal.
    didi::godot::EditorHookTestAccess::setSessionKind(hook, didi::runtime::SessionKind::game);
    for (const auto* method : {"anim.listTracks", "anim.playTrack"}) {
        // The play was missing from the game gate on the first live run and
        // came back as an editor-only refusal; both must reach the bridge.
        auto admitted = didi::godot::EditorHookTestAccess::executeOnMainThread(
            hook, method, {{"animation_player_path", "/root/Player"}, {"animation_name", "walk"}});
        ASSERT_TRUE(admitted.contains("error"));
        ASSERT_TRUE(admitted["error"]["message"] != "session_kind_rejected");
        ASSERT_TRUE(admitted["error"]["message"].get<std::string>().find("Editor-only method") == std::string::npos);
    }
    didi::godot::EditorHookTestAccess::setSessionKind(hook, std::nullopt);
}

struct RegisterPhase7bAnim {
    RegisterPhase7bAnim() {
        registerTest("phase7b_anim.registry", test_registry_advertises_list_read_and_play_mutation);
        registerTest("phase7b_anim.requests", test_requests_parse_and_reject);
        registerTest("phase7b_anim.catalog_sort_and_enums", test_catalog_sorts_names_and_maps_enums);
        registerTest("phase7b_anim.catalog_count_caps", test_catalog_applies_count_caps_with_a_cursor);
        registerTest("phase7b_anim.catalog_byte_budget", test_catalog_stops_before_a_record_at_the_byte_budget);
        registerTest("phase7b_anim.hook_policy", test_hook_policy_for_list_and_play);
    }
} g_registerPhase7bAnim;

} // namespace
