#pragma once

// Standalone stand-in for the one database type the reused GUI kit references:
// Feedback::showRowResult() switches on db::RowResult. GitWallet has no
// database, but the enum is kept so the shared Feedback helper compiles and can
// still be used for row-style confirmations if a future page wants them.
namespace app::db {

enum class RowResult {
    Ok,       // the row was changed / removed
    NotFound, // the row was already gone (stale click) — not an error
    Error,    // the change failed
};

} // namespace app::db
