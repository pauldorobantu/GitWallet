#pragma once

#include <QString>

namespace app::ui {
class MainWindow;
}

namespace app {

// --scale-test: the real window with this many made-up rows, what each thing a user does
// takes. Offline, on a throw-away --data-dir; returns the timings to print.
QString runWindowScaleTest(ui::MainWindow* window, int rows);

} // namespace app
