#pragma once

#include <QLoggingCategory>

// Standalone stand-in for FlowSuite's core/AppLogger: the reused GUI kit logs
// user-visible errors to the "local" category (Feedback::show). Here it is a
// plain Qt logging category; output goes wherever Qt's message handler sends it.
namespace app {

Q_DECLARE_LOGGING_CATEGORY(lcLocal)

} // namespace app
