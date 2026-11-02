#pragma once

#include "wallet/Types.h"

#include <QWidget>

#include <optional>

namespace app {
class WalletController;
}

namespace app::ui {

// The Receive page: a wallet and a coin, and the next address nobody has been given, as a QR code
// and as text. Copying it or saving its QR marks it as given out, and it is not offered again; New
// address goes on to the next. What was given out is listed under it with what came of it, as a
// business hands out one address per payment. The address is derived from the key each time it is
// shown, and shown only if the key and what is kept agree.
class ReceivePage : public QWidget
{
    Q_OBJECT
public:
    explicit ReceivePage(WalletController* controller, QWidget* parent = nullptr);

    // Comes up on this wallet and coin; 0 and nullopt: keeps what is chosen.
    void showFor(quint32 wallet, std::optional<wallet::Asset> asset);
    // One address, as it is, whatever has come of it. Nothing is marked by showing it.
    void showAddress(const wallet::AddressRow& row);

    // What is on screen, for tests.
    QString shownAddress() const;
    QString shownPayload() const; // what the QR code says

signals:
    void openAddressRequested(const wallet::AddressRow& row);

protected:
    void showEvent(QShowEvent* event) override;

private:
    struct Ui;

    void rebuildWallets();
    void rebuildCoins();
    void rebuildTypes();
    void offerNext();     // what the choices above say comes next
    void updateShown();   // the QR, the text and the state of what is chosen
    void updateList();
    void giveOut();       // the address shown left this screen
    void newAddress();
    void saveQr();
    bool keepLabel();     // false + a line on screen when the label cannot be kept
    QString payload() const;
    wallet::Network chosenNetwork() const;
    chain::ScriptType chosenType() const;

    WalletController* controller_ = nullptr;
    Ui* ui_ = nullptr;
};

} // namespace app::ui
