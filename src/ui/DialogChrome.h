#pragma once

class QDialog;

namespace app::ui {

// Aceeași „formă" ca fereastra principală pt. orice fereastră de dialog: fără
// rama nativă Windows, bară de titlu integrată în temă (titlu + închidere, se
// trage cu mouse-ul), ramă fină de 1px în culoarea temei (rolul „frame"),
// colțuri drepte. Marginile de redimensionare rămân pe laturi (dacă dialogul
// nu are mărime fixă). Se aplică o singură dată, ÎNAINTE de prima afișare.
void applyDialogChrome(QDialog* dialog);

// Înălțimea barei de titlu a dialogurilor (= bara ferestrei principale).
constexpr int kDialogTitleHeight = 38;

} // namespace app::ui
