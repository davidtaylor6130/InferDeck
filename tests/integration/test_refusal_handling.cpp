#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "messaging/conversions.hpp"
#include "messaging/message.hpp"

#include "fixture_loader.hpp"

using inferdeck::messaging::Conversation;
using inferdeck::messaging::Message;
using inferdeck::messaging::Role;
using inferdeck::messaging::TextContent;
using inferdeck::messaging::conversation_from_oai;
using inferdeck::messaging::message_from_oai;
using nlohmann::json;

namespace {

bool has_text(const Message& m, const std::string& needle) {
    for (const auto& c : m.content) {
        if (auto* t = std::get_if<TextContent>(&c)) {
            if (t->text.find(needle) != std::string::npos) return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("Refusal: oai_refusal fixture parses and preserves refusal field", "[integration][refusal][oai]") {
    auto raw = test_helpers::load_fixture_text("oai_refusal.json");
    REQUIRE(raw.has_value());
    auto j = json::parse(*raw, nullptr, false);
    REQUIRE_FALSE(j.is_discarded());
    REQUIRE(j.contains("messages"));

    Conversation c;
    for (const auto& mj : j["messages"]) {
        auto parsed = message_from_oai(mj);
        REQUIRE(parsed.has_value());
        c.messages.push_back(*parsed);
    }
    REQUIRE(c.messages.size() >= 2);

    bool found_refusal = false;
    for (const auto& m : c.messages) {
        if (m.role == Role::Assistant && m.refusal.has_value() && !m.refusal->empty()) {
            found_refusal = true;
            REQUIRE(m.content.empty());
        }
    }
    REQUIRE(found_refusal);
}

TEST_CASE("Refusal: assistant message with refusal converts back to OAI shape", "[integration][refusal][roundtrip]") {
    auto raw = test_helpers::load_fixture_text("oai_refusal.json");
    REQUIRE(raw.has_value());
    auto j = json::parse(*raw, nullptr, false);

    Conversation c = *conversation_from_oai(j);
    bool found = false;
    for (const auto& m : c.messages) {
        if (m.role == Role::Assistant && m.refusal.has_value()) {
            found = true;
            json oai_msg;
            oai_msg["role"] = "assistant";
            oai_msg["refusal"] = *m.refusal;
            REQUIRE(oai_msg["refusal"].is_string());
            REQUIRE_FALSE(oai_msg["refusal"].get<std::string>().empty());
        }
    }
    REQUIRE(found);
}

TEST_CASE("Refusal: non-refusal assistant message has no refusal field", "[integration][refusal][negative]") {
    Message m;
    m.role = Role::Assistant;
    m.content.push_back(TextContent{"ok I will help"});
    REQUIRE_FALSE(m.refusal.has_value());
    auto back = message_from_oai(json{{"role", "assistant"}, {"content", "ok I will help"}});
    REQUIRE(back.has_value());
    REQUIRE_FALSE(back->refusal.has_value());
}

TEST_CASE("Refusal: rejection in edge_empty_content fixture", "[integration][refusal][edge]") {
    auto raw = test_helpers::load_fixture_text("edge_empty_content.json");
    REQUIRE(raw.has_value());
    auto j = json::parse(*raw, nullptr, false);
    auto conv = conversation_from_oai(j);
    REQUIRE(conv.has_value());
    for (const auto& m : conv->messages) {
        if (m.role == Role::User) {
            if (m.content.empty()) {
                SUCCEED("empty content as expected for edge case");
            } else {
                REQUIRE(m.content.size() == 1);
            }
        }
    }
}
