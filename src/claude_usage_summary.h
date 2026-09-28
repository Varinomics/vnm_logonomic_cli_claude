#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>

namespace claude {

void handle_usage_summary(logonomic::cli_addon::Invocation_context& context);
void handle_usage_daily(logonomic::cli_addon::Invocation_context& context);

} // namespace claude
