#pragma once

#include "wallet/Types.h"

#include <QWidget>

#include <functional>
#include <optional>

class QFrame;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QTabBar;
class QTimer;

namespace app {
class WalletController;
}

namespace app::ui {

// Where the reader is, on the way down from a total: the same screens are reached from a coin
// and from a wallet, down to one address.
struct Place {
    enum class Root { Portfolio, Wallets, Addresses };
    enum class Tab { Holdings, Addresses, History };

    Root root = Root::Portfolio;
    std::optional<wallet::Asset> asset;
    quint32 wallet = 0;        // 0: every wallet
    bool onlyWallet = false;   // the one there is: the path has no need to name it
    std::optional<wallet::AddressRow> address;

    // What the tab beside Addresses and History holds, if there is one.
    bool hasHoldings() const { return address.has_value() || !asset.has_value() || wallet == 0; }
    Tab firstTab() const { return hasHoldings() ? Tab::Holdings : Tab::Addresses; }
    bool hasAddressesTab() const { return !address.has_value(); }
    // The network this all is on, if it is on one.
    std::optional<wallet::Network> network() const;
};

// The clickable path along the top, what the place holds, and a tab for each way to look at it.
// Every page below keeps its own figures: this only says where they are from.
class PlaceBar : public QWidget
{
    Q_OBJECT
public:
    explicit PlaceBar(WalletController* controller, QWidget* parent = nullptr);

    void showPlace(const Place& place, Place::Tab tab);
    void refresh();
    Place place() const { return place_; }
    Place::Tab tab() const;
    QStringList pathTexts() const; // for tests

signals:
    void placeChosen(const app::ui::Place& place); // a step of the path
    void tabChosen(app::ui::Place::Tab tab);
    void rootChosen(app::ui::Place::Root root);
    void receiveRequested(const app::ui::Place& place);
    void showAddressQr(const app::ui::Place& place);
    void labelRequested(const QString& address);

private:
    void rebuildPath();
    void rebuildTabs(Place::Tab tab);
    void addStep(const QString& text, const std::function<void()>& open);

    WalletController* controller_ = nullptr;
    Place place_;
    QHBoxLayout* path_ = nullptr;
    QLabel* mark_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* detail_ = nullptr;
    QPushButton* copy_ = nullptr;
    QPushButton* explorer_ = nullptr;
    QPushButton* label_ = nullptr;
    QPushButton* qr_ = nullptr;
    QPushButton* receive_ = nullptr;
    QTabBar* tabs_ = nullptr;
    QTimer* refresh_ = nullptr;
    QList<Place::Tab> tabOrder_;
};

} // namespace app::ui
