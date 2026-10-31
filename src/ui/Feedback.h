#pragma once

#include <QString>

class QWidget;

namespace app::db {
enum class RowResult;
}

namespace app::ui {

// Feedback de acțiune LÂNGĂ CURSOR (înlocuiește toast-urile din colțul
// dreapta-jos): un puls discret în culoarea stării acolo unde s-a dat click +
// o pastilă mică (icon + text scurt) lângă pointer, care se stinge singură.
// Una singură odată (o nouă confirmare o înlocuiește pe cea veche), transparentă
// la mouse — nu întrerupe și nu acoperă nimic de durată.
class Feedback
{
public:
    enum class Type { Success, Warning, Error, Info };

    // `context`: orice widget din fereastra în care s-a făcut acțiunea (sau null
    // → fereastra activă).
    static void show(QWidget* context, Type type, const QString& message);

    // Standard outcome of a single-row action (delete / restore / toggle):
    // Ok → Success `doneMessage`; NotFound → Info "<subject> was already
    // removed" (stale row, double click — not an error); Error → Error
    // "Could not <verb> <subject>". Returns true when the caller should
    // refresh (the row is gone or changed: Ok / NotFound).
    static bool showRowResult(QWidget* context, db::RowResult result,
                              const QString& doneMessage, const QString& verb,
                              const QString& subject);
};

} // namespace app::ui
