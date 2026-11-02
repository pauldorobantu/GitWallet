#pragma once

#include "wallet/Registry.h"

#include <QWidget>

#include <optional>

namespace app {
class WalletController;
}

namespace app::ui {

class HistoryTableModel;

// The History page: every transaction of every wallet, newest first, with what it did in the
// coin and in USD now and then, its fee and how far it is believed; filters by wallet, type,
// state and period, a search over ids, addresses, labels and notes, and an export of what those
// leave. It narrows to a wallet, a network or one address, which PlaceBar then says. Opening a
// line shows the transaction whole.
class HistoryPage : public QWidget
{
    Q_OBJECT
public:
    // What Copy gives of a cell: what it means, whole. Empty: it says there is nothing.
    static QString textToCopy(const QModelIndex& cell);

    explicit HistoryPage(WalletController* controller, QWidget* parent = nullptr);

    void showAll();
    void showWallet(quint32 id);
    // Part of a place: to a wallet, a network, one address, or what they leave, and the combo that
    // would say otherwise goes. A network with no history here yet says so.
    void showPlace(quint32 wallet, std::optional<wallet::Network> network, const QString& scripthash, const QString& address);
    void clearPlace();

    // The lines the filters leave, in the table's order, at once. False + a sentence on failure.
    bool exportCsvNow(const QString& path, QString* error);
    // A transaction opened as if double-clicked. For tests.
    void openRow(int row);

    HistoryTableModel* tableModel() const;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    struct Ui;

    void applyColumnWidths();
    void rebuildWalletFilter();
    void resetFilters();
    bool periodMoved() const; // what the period chosen covers is no longer what the list goes by
    void applyFilter();
    void openSelected();
    void chooseExportFile();
    void exportStep();
    void finishExport(bool ok, const QString& error);
    QString headerLine() const;
    bool writeLines(int from, int to, QString* error);
    void updateState();
    void rememberPlace();
    void restorePlace();

    WalletController* controller_ = nullptr;
    Ui* ui_ = nullptr;
};

} // namespace app::ui
