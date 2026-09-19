#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/helper_ingress.h>

namespace claude {

bool canonical_session_id(std::string_view value);
logonomic::cli_addon::Classify_event_result classify_event(
    const logonomic::cli_addon::Classify_event_request& request);
void handle_lifecycle(logonomic::cli_addon::Invocation_context& context);
logonomic::cli_addon::Attention_classification_result classify_attention(
    const logonomic::cli_addon::Attention_classification_request& request);

} // namespace claude
