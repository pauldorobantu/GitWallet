#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;

namespace app {
class WalletController;
}

namespace app::ui {

class HoldingsTable;

// What a coin, a wallet or an address holds: the table the first tab of a place shows (PlaceBar).
class HoldingsPage : public QWidget
{
    Q_OBJECT
public:
    explicit HoldingsPage(WalletController* controller, QWidget* parent = nullptr);

    HoldingsTable* table() const { return table_; }

protected:
    void showEvent(QShowEvent* event) override; // the box follows the one on the Portfolio

private:
    void updateCount();

    WalletController* controller_ = nullptr;
    HoldingsTable* table_ = nullptr;
    QCheckBox* hideZero_ = nullptr;
    QCheckBox* hideDust_ = nullptr;
    QLabel* count_ = nullptr;
};

} // namespace app::ui
