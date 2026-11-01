#pragma once

#include <QWidget>

namespace app {
class WalletController;
}

namespace app::ui {

// The Addresses page: every derived address in one table (chain, type, path,
// address, balance, USD, state), with a search field, a chain filter
// (All / Bitcoin / Ethereum), a state filter (All / Funded / Used) and
// click-to-copy on the address column. Shows the kit's empty / no-results
// states. New addresses appear as the scanners add them.
class AddressesPage : public QWidget
{
    Q_OBJECT
public:
    explicit AddressesPage(WalletController* controller, QWidget* parent = nullptr);

private:
    WalletController* controller_ = nullptr;

    // --- implementation (AddressesPage.cpp) ---
    struct Ui; // the page's widgets and models

    void applyColumnWidths();
    void keepTopRow(int first, int last);
    void scheduleUpdate();
    void updateState(); // count, enabled controls, table / empty / no-results

    Ui* ui_ = nullptr;
};

} // namespace app::ui
