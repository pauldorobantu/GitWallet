#pragma once

// Standalone stand-in for FlowSuite's diag/Diag. The reused Feedback.cpp
// reports row-action failures through APP_DIAG_ERROR; GitWallet has no
// diagnostics backend, so the macros compile to nothing.
#define APP_DIAG_ERROR(...) do { } while (0)
#define APP_DIAG_WARN(...)  do { } while (0)
#define APP_DIAG_INFO(...)  do { } while (0)
