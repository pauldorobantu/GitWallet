#pragma once

#include "wallet/Registry.h"

#include <QWidget>

namespace app {
class WalletController;
}

namespace app::ui {

class HistoryTableModel;

// The History page: every transaction of every wallet, newest first, with what it did in the
// coin and in USD now and then, its fee and how far it is believed; filters by wallet, type,
// state and period, a search over ids, addresses, labels and notes, and an export of what those
// leave. It narrows to one wallet or one address, and then says what that holds. Opening a line
// shows the transaction whole.
class HistoryPage : public QWidget
{
    Q_OBJECT
public:
    explicit HistoryPage(WalletController* controller, QWidget* parent = nullptr);

    void showAll();
    void showWallet(quint32 id);
    // One address: what it holds, then its transactions.
    void showAddress(wallet::Network network, const QString& scripthash, const QString& address);

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
    void applyFilter();
    void openSelected();
    void chooseExportFile();
    void exportStep();
    void finishExport(bool ok, const QString& error);
    QString headerLine() const;
    bool writeLines(int from, int to, QString* error);
    void updateState();
    void updateScope();
    void rememberPlace();
    void restorePlace();

    WalletController* controller_ = nullptr;
    Ui* ui_ = nullptr;
};

} // namespace app::ui
