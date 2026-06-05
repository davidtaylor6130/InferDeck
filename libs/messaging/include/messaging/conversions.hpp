#pragma once

#include <nlohmann/json.hpp>

#include "foundation/result.hpp"
#include "messaging/message.hpp"

namespace inferdeck::messaging {

using foundation::Result;
using foundation::Ok;
using foundation::Err;
using foundation::ErrorCode;
using foundation::Error;

using Json = nlohmann::json;

[[nodiscard]] Json content_to_oai(const Content& c);
[[nodiscard]] Result<Content> content_from_oai(const Json& j);

[[nodiscard]] Json message_to_oai(const Message& m);
[[nodiscard]] Result<Message> message_from_oai(const Json& j);

[[nodiscard]] Json conversation_to_oai(const Conversation& c);
[[nodiscard]] Result<Conversation> conversation_from_oai(const Json& j);

[[nodiscard]] Json content_to_anthropic(const Content& c);
[[nodiscard]] Result<Content> content_from_anthropic(const Json& j);

[[nodiscard]] Json message_to_anthropic(const Message& m);
[[nodiscard]] Result<Message> message_from_anthropic(const Json& j);

[[nodiscard]] Json conversation_to_anthropic(const Conversation& c);
[[nodiscard]] Result<Conversation> conversation_from_anthropic(const Json& j);

[[nodiscard]] Json content_to_internal(const Content& c);
[[nodiscard]] Result<Content> content_from_internal(const Json& j);

[[nodiscard]] Json message_to_internal(const Message& m);
[[nodiscard]] Result<Message> message_from_internal(const Json& j);

} // namespace inferdeck::messaging
