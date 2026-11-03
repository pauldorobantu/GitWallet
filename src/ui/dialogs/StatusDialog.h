#pragma once

#include <QDialog>
#include <QList>

class QLabel;
class QTimer;

namespace app {
class WalletController;
}

namespace app::ui {

// How the scan stands and on which servers, opened from the title bar's status. It follows along while
// it is open: the scanners go on behind it.
class StatusDialog : public QDialog
{
    Q_OBJECT
public:
    explicit StatusDialog(WalletController* controller, QWidget* parent = nullptr);

private:
    void refresh();

    WalletController* controller_ = nullptr;
    QLabel* scan_ = nullptr;
    QLabel* whole_ = nullptr;
    QLabel* wallets_ = nullptr;
    QLabel* scanned_ = nullptr;
    QLabel* used_ = nullptr;
    QLabel* funded_ = nullptr;
    QList<QLabel*> servers_; // one per wallet::networks() entry, in that order
    QTimer* soon_ = nullptr;
};

} // namespace app::ui
