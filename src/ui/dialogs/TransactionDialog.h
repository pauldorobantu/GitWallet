#pragma once

#include "ui/HistoryTableModel.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTimer;

namespace app {
class WalletController;
}

namespace wallet {
struct TxDetail;
}

namespace app::ui {

// One transaction whole: how far it is believed and why, which follows the book for as long as it
// is open, what it did in the coin, in USD now and when it happened, its fee, where its coins came
// from and went, an explorer link, and the label and note only this PC knows. Those are kept as the
// window closes, and if they cannot be it stays open and says why: what was typed exists nowhere else.
class TransactionDialog : public QDialog
{
    Q_OBJECT
public:
    TransactionDialog(WalletController* controller, HistoryTableModel* model, const HistoryTableModel::Line& line, QWidget* parent = nullptr);

    void done(int result) override;

private:
    void showDetail(const wallet::TxDetail& detail);
    void showFee(qint64 fee, int vsize);
    void refresh(); // what it says of the transaction, as the book has it now
    bool saveNotes();
    void addLine(QTableWidget* table, const QString& address, qint64 value, bool ours, bool known);

    WalletController* controller_ = nullptr;
    HistoryTableModel* model_ = nullptr;
    HistoryTableModel::Line line_;
    chain::Hash256 txid_{};
    QTimer* follow_ = nullptr;
    QLabel* kindValue_ = nullptr;
    QLabel* stateValue_ = nullptr;
    QLabel* whyValue_ = nullptr;
    QLabel* dateValue_ = nullptr;
    QLabel* amountValue_ = nullptr;
    QLabel* usdNowValue_ = nullptr;
    QLabel* usdThenValue_ = nullptr;
    QLabel* notKept_ = nullptr;
    QPushButton* discard_ = nullptr;
    bool discarding_ = false;
    qint64 detailFee_ = -1; // what the detail worked out, of a payment that is not the wallets' too
    int detailVsize_ = 0;
    QLabel* feeValue_ = nullptr;
    QLabel* reading_ = nullptr;
    QTableWidget* from_ = nullptr;
    QTableWidget* to_ = nullptr;
    QLineEdit* label_ = nullptr;
    QPlainTextEdit* note_ = nullptr;
    QString labelAtOpen_;
    QString noteAtOpen_;
};

} // namespace app::ui
