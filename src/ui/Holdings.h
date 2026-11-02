#pragma once

#include "chain/Amount.h"
#include "wallet/Registry.h"
#include "wallet/Types.h"

#include <QColor>
#include <QList>
#include <QWidget>

class QTableView;
class QTimer;

namespace app {
class WalletController;
}

namespace app::ui {

// What a scope (everything, or one wallet) holds of one asset, and how far that can be believed
// now. A figure reads plain only once every balance behind it has answered: a "0" for an address
// nobody has answered for would claim the funds are gone. Until then it reads "—" while nothing
// is known, and "≥ X" for what is known so far.
struct Holding {
    wallet::Asset asset{};
    chain::Amount amount;
    double price = 0.0;     // of one unit; 0: none known
    double usd = 0.0;       // at that price
    bool watched = false;   // a key in the scope serves its network; if none does nothing was looked at, and it is no zero
    bool complete = false;  // every balance behind it has answered and the scan reached all it should
    bool readable = true;   // chain:: could convert it
    int holders = 0;        // addresses holding some
    int pending = 0;        // addresses of its network without an answer yet
    bool missing = false;   // an account has no rows yet
    bool unread = false;    // a key of a kind this version cannot read
    bool oneWallet = false; // of one wallet, rather than of them all

    bool holds() const { return !amount.isZero(); }
    QString amountText() const;  // "0.5 BTC", "≥ 0.5 BTC", "—"
    QString usdText() const;     // "$1.00", "≥ $1.00", "—"
    QString holdersText() const; // "3", "≥ 3", "—"
};
Holding holdingOf(const WalletController* controller, wallet::Asset asset, quint32 wallet = 0);
// The same of one address.
Holding holdingAt(const WalletController* controller, const wallet::AddressRow& row, wallet::Asset asset);
// Why a figure is not final, and how current what is known of it is. Empty: nothing to say.
QString holdingNote(const WalletController* controller, const Holding& holding);

// The sum of a scope in USD. Something held without a price would make it silently too low, so
// then there is no sum.
struct ScopeTotal {
    double usd = 0.0;
    bool known = false;    // there is a sum to show: every asset held has a price
    bool complete = false; // and nothing behind it is still unknown
    QString text() const;  // "$1.00", "≥ $1.00", "—"
};
ScopeTotal totalOf(const WalletController* controller, quint32 wallet = 0);

// A thin bar of what a total is made of, in the coins' colours.
class AllocationBar : public QWidget
{
public:
    struct Part {
        QColor color;
        double usd = 0.0;
        QString name;
    };
    explicit AllocationBar(QWidget* parent = nullptr);
    void setParts(const QList<Part>& parts);
    QList<Part> parts() const { return parts_; }

protected:
    void paintEvent(QPaintEvent* event) override;
    bool event(QEvent* event) override; // says which part is under the pointer

private:
    QList<Part> parts_;
    double total_ = 0.0;
};

// One row per coin, or per wallet that holds a coin, or per asset of one address: what opens
// downward from a total. Worth first, whichever it is; a header click sorts it another way. One
// click opens a row, as in the wallets people know.
class HoldingsTable : public QWidget
{
    Q_OBJECT
public:
    explicit HoldingsTable(WalletController* controller, QWidget* parent = nullptr);

    void showCoins(quint32 wallet = 0);     // every coin of every wallet, or of one
    void showWallets(wallet::Asset asset);  // the wallets that have a key for it
    void showAddress(const wallet::AddressRow& row); // every asset of its network
    // Rows known to hold nothing go, unless that is all there is.
    void setHideZero(bool hide);
    bool hideZero() const;

    QTableView* view() const;
    int rowsShown() const;
    int rowsHolding() const; // of those a key looks at
    int rowsTotal() const;   // those a key looks at
    bool allFinal() const;   // every figure among those has answered
    void refresh();

    // What each coin comes to now, for an AllocationBar.
    QList<AllocationBar::Part> parts() const;

signals:
    void coinChosen(wallet::Asset asset);
    void walletChosen(quint32 wallet);
    void refreshed();

protected:
    void showEvent(QShowEvent* event) override;

private:
    struct Ui;
    void applyKind();
    void queueRefresh();
    void opened(); // it came up on something else

    WalletController* controller_ = nullptr;
    Ui* ui_ = nullptr;
};

} // namespace app::ui
