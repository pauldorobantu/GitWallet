#pragma once

#include <QDialog>

class QPushButton;
class QTableView;

namespace app {
class WalletController;
}

namespace wallet {
struct AddressRow;
}

namespace app::ui {

// What came in, the latest first: when, how much and what it was worth then, on which wallet and
// address, and how that payment stands now. What had not been looked at reads bold; closing this says
// it has been. It goes on filling while it is open. A row opens on its address.
class PaymentsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PaymentsDialog(WalletController* controller, QWidget* parent = nullptr);

    QTableView* view() const { return view_; }
    void done(int result) override;
    // The lock is what closes it: nobody looked at what came after it opened.
    void closingForLock() { forLock_ = true; }

signals:
    void openAddressRequested(const wallet::AddressRow& row);

private:
    void openChosen();
    void updateState();

    WalletController* controller_ = nullptr;
    QTableView* view_ = nullptr;
    QPushButton* open_ = nullptr;
    QWidget* nothing_ = nullptr;
    qint64 lastAtOpen_ = 0;
    bool forLock_ = false;
};

} // namespace app::ui
