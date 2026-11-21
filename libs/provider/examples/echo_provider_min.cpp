#include <cstdint>

#include "inferdeck/provider_abi.h"

namespace {

int32_t create(const char*, const inferdeck_call_v2*, void** instance)
{
    *instance = instance;
    return INFERDECK_OK;
}

void destroy(void*) {}
int32_t lifecycle(void*, const inferdeck_call_v2*) { return INFERDECK_OK; }

int32_t invoke(void*, const char*, const char*, const inferdeck_blob*, size_t, const inferdeck_call_v2* call)
{
    call->reply(call->context, "{}", nullptr, 0);
    return INFERDECK_OK;
}

int32_t unreachable_provider_invoke(const char*, const char*, const inferdeck_blob*, size_t, const inferdeck_call_v2*)
{
    return INFERDECK_INTERNAL;
}

inferdeck_provider_v2 g_table = {
    INFERDECK_PROVIDER_V2_MIN_SIZE, INFERDECK_PROVIDER_ABI_VERSION_2,
    R"({"runtime":"echo_min","version":"1.0.0","modalities":{"text":["chat_completions"]},"model_source":"none"})",
    nullptr, nullptr, create, destroy, lifecycle, lifecycle, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, invoke, unreachable_provider_invoke};

}

extern "C" INFERDECK_PROVIDER_EXPORT const struct inferdeck_provider_v2* inferdeck_provider_v2(void)
{
    return &g_table;
}
