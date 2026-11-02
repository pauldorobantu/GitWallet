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

    // Comes up on what is next, on this wallet and coin; 0 and nullopt: on what is chosen.
    void showFor(quint32 wallet, std::optional<wallet::Asset> asset);
    // One address, as it is, whatever has come of it, until told otherwise. Showing it marks nothing:
    // letting it leave the screen does.
    void showAddress(const wallet::AddressRow& row);
    // Writes the QR code to a file, as Save QR does once a file is chosen.
    bool saveQrAs(const QString& path);

    // What is on screen, for tests.
    QString shownAddress() const;
    QString shownPayload() const; // what the QR code says
    QString lastCopied() const;   // what Copy address last handed over

signals:
    void openAddressRequested(const wallet::AddressRow& row);

private:
    struct Ui;

    void rebuildWallets();
    void rebuildCoins();
    void rebuildTypes();
    void chosenChanged(); // a combo moved: what it says comes up
    void offerNext();     // what the choices above say comes next, unless what is shown stays
    void resetTyped();    // the amount and the label were for what was on screen
    void loadLabel();     // what the address on screen is called, unless something else was typed
    void updateShown();   // the QR, the text and the state of what is on screen
    void updateList();
    bool giveOut();       // the address shown left this screen; false: a label typed for it was refused
    void newAddress();
    void saveQr();
    bool keepLabel();     // false + a line on screen when the label cannot be kept
    int shownPosition() const; // of what is on screen in the model; -1 if it has gone
    QString payload() const;
    wallet::Network chosenNetwork() const;
    chain::ScriptType chosenType() const;

    WalletController* controller_ = nullptr;
    Ui* ui_ = nullptr;
};

} // namespace app::ui
