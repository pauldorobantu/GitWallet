#pragma once

#include "wallet/Types.h"

#include <QWidget>

#include <optional>

namespace app {
class WalletController;
}

namespace app::ui {

// The Addresses page: every derived address of every wallet in one table (wallet,
// chain, type, path, address, balance, USD, state), with a search field, a wallet
// filter, a chain filter (All / Bitcoin / Ethereum), a state filter (All / Funded
// / Used / Given out) and a CSV export of what the filters leave. Shows the kit's
// empty / no-results states. New addresses appear as the scanners add them. One
// click chooses a row and a double click opens it; Copy address or Ctrl+C hands it
// out, which marks it, so that nothing else may.
class AddressesPage : public QWidget
{
    Q_OBJECT
public:
    explicit AddressesPage(WalletController* controller, QWidget* parent = nullptr);

    // Narrows the table to one wallet (0: all of them).
    void showWallet(quint32 id);
    // Part of a place: to a wallet, a coin or both, and the combos that would say otherwise go.
    // On a coin, the addresses that hold some of it come up, and State says the rest are there.
    void showPlace(quint32 wallet, std::optional<wallet::Asset> asset);
    void clearPlace();
    void editLabel(const QString& address);
    // The rows the filters leave, in the table's order, as CSV (UTF-8 with a BOM,
    // for Excel). False + a sentence when the file cannot be written.
    bool exportCsv(const QString& path, QString* error) const;
    // What Copy address last handed over, for tests.
    QString lastCopied() const;

signals:
    void openAddressRequested(const wallet::AddressRow& row);
    void showHistoryRequested(const wallet::AddressRow& row);

protected:
    void showEvent(QShowEvent* event) override; // what a place left narrowed goes back as the page shows

private:
    WalletController* controller_ = nullptr;

    // --- implementation (AddressesPage.cpp) ---
    struct Ui; // the page's widgets and models

    void becomeWhole();

    void applyColumnWidths();
    void rebuildWalletFilter(); // the combo follows the wallet list; one wallet needs neither it nor the column
    void chooseExportFile();
    void openHistoryOf(const QModelIndex& index);
    void openAddressOf(const QModelIndex& index);
    void editLabelOfSelected();
    void copySelected();
    void keepTopRow(int first, int last);
    void scheduleUpdate();
    void updateState(); // count, enabled controls, table / empty / no-results

    Ui* ui_ = nullptr;
};

} // namespace app::ui
