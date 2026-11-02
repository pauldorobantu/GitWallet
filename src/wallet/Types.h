#pragma once

#include "chain/Address.h"
#include "chain/Amount.h"
#include "chain/Bip32.h"
#include "wallet/Registry.h"

#include <QDateTime>
#include <QJsonArray>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

// Shared value types of the wallet layer.
namespace wallet {

// One stored account: the public node at m/purpose'/coin'/account'.
struct AccountKey {
    quint32 wallet = 0; // WalletEntry::id of its wallet; the entry's position in the file says it
    chain::ScriptType type = chain::ScriptType::P2WPKH;
    Network network{};  // a UTXO type's network; an Evm account serves every Evm network
    quint32 account = 0;
    QString xpub; // serialized with chain::xpubVersionFor(type)
    quint32 issued = 0;     // receive addresses below this index are never offered again
    quint32 issuedFrom = 0; // the first one Receive gave out: before it, what was used was used elsewhere

    // Parses `xpub`; throws std::runtime_error if it is not a valid extended key.
    chain::HdPubKey node() const;
    bool serves(Network other) const;
    const chain::AddressFormat& format() const;
};

// One wallet of the file: the accounts of a seed, or public keys added on their
// own. Either way only public keys are kept, so no wallet can spend from here.
struct WalletEntry {
    quint32 id = 0;        // stable and never 0; keys and rows refer to it
    QString label;         // the name the user gave it
    bool fromKeys = false; // added as public keys, without a seed's words
    QList<AccountKey> accounts;
    QJsonArray keptAccounts; // of a type or network this version does not know: stays in the file, unread
};

// A wallet to add, as a batch asks for it. `refused` says why it was not added, if it
// was not.
struct WalletRequest {
    QList<AccountKey> accounts;
    QString label;         // empty: "Wallet N"
    bool numbered = false; // `label` is what the name starts with: the next free number follows
    bool fromKeys = false;
    int line = 0;          // the caller's: which line of its list this came from
    QString refused;
};

enum class AddressState { Unknown, Unused, Used, Funded };

// How far the reading of every nonce of a network has got. They cost too much to start over
// each time, and a wallet opened for a few minutes at a time still has to get through them.
struct NonceSweep {
    QDateTime from;      // when it began; invalid: none under way
    quint32 wallet = 0;  // the row it goes on from
    quint32 account = 0;
    quint32 index = 0;
    bool clears = false; // the endpoints changed: what an earlier one said of an address goes too
};

// An address to give to whoever is to pay: the first of a receive chain that has no history and
// that nobody has been given.
struct ReceiveOffer {
    enum class State {
        Ready,    // `address` comes from the key, just now
        Checking, // the next one has no answer yet, so nobody can say it is unused
        NoKey,    // the wallet has no key for this
        Failed,   // the row kept for it is not what the key gives: `why`
    };
    State state = State::NoKey;
    QString address;
    QString path;
    quint32 account = 0;
    quint32 index = 0;
    QString why;
};

// What a scanner's problem report is about, so the UI reacts to the kind and
// never has to parse the wording.
enum class Problem {
    Failure,        // a lookup failed and is retried; balances may be incomplete meanwhile
    Limit,          // a chain or account reached the row limit; later addresses are not scanned
    WrongChain,     // a server does not answer for its network (first block, chain id, no Multicall3, ...)
    BuiltInServers, // none of the Electrum servers in Settings works; the built-in list is in use
    Skipped,        // an account or chain cannot be scanned at all
    FileSetAside,   // a wallet.dat that could not be read was kept under another name
    SavedData,      // the database of what the wallets have shown cannot be read or written
    SavedDataAside, // a database that could not be read was kept under another name, and a new one is in use
    FilesLeft,      // files that hold keys or addresses could not be deleted
    FolderHeld,     // another window holds the data folder, so nothing is saved here for now
    LabelsNotKept,  // labels and notes, which exist nowhere else, are not going into the database
    LabelsOwed,     // those of a file set aside have yet to come over, and until they have none can change
};

// One address on one network. An Evm address exists on every Evm network, with one row
// per network, as what it holds, its nonce and whether it answered differ per network.
struct AddressRow {
    quint32 wallet = 0; // WalletEntry::id
    Network network{};
    chain::ScriptType type = chain::ScriptType::P2WPKH;
    quint32 account = 0;
    quint32 change = 0; // 0 receive, 1 change (UTXO); always 0 for an Evm account
    quint32 index = 0;
    QString address;
    QString scripthash; // UTXO: the Electrum scripthash; empty for an Evm account

    bool checked = false; // every lookup has answered: UTXO the status, plus the balance
                          // of a used address; Evm the nonce and every balance
    bool used = false;    // UTXO: has history; Evm: a nonce or a balance, now or before (EvmScanner says when that ends)

    // UTXO: the coin's balance, in smallest units.
    qint64 confirmed = 0;
    qint64 unconfirmed = 0; // negative while an outgoing tx is unconfirmed
    // Evm: every asset it holds, in smallest units. A zero is not kept.
    QMap<Asset, chain::Amount> balances;
    quint64 nonce = 0;  // Evm: outgoing transaction count
    QString status;     // UTXO: the last status hash of its history; empty if none is known

    bool isUtxo() const { return chain::isUtxo(type); }
    QString path() const; // "m/84'/0'/0'/0/5"
    // Funded if any balance > 0; else Used if it has history (checked or not);
    // else Unused once checked; else Unknown.
    AddressState state() const;
    // This row's balance in `a` (UTXO: confirmed + unconfirmed, floored at 0; zero for
    // assets of another network).
    chain::Amount amount(Asset a) const;
    // Sets an Evm balance, keeping balances free of zeros.
    void setBalance(Asset a, const chain::Amount& value);
    // Calls `f(asset, amount)` for what the row holds.
    template <typename F>
    void forEachBalance(F f) const
    {
        if (isUtxo()) {
            const Asset coin = networkInfo(network).coin;
            const chain::Amount value = amount(coin);
            if (!value.isZero())
                f(coin, value);
            return;
        }
        for (auto it = balances.cbegin(); it != balances.cend(); ++it)
            f(it.key(), it.value());
    }
};

// What a network's scan asks of the servers, and how far behind that can leave a figure.
struct ScanTiers {
    int live = 0;        // rows followed live, or read in every short round
    int rounds = 0;      // the other rows, read in long rounds
    int roundSeconds = 0; // how long a long round takes; 0 while none has run or is planned
};

struct ScanSettings {
    int utxoReceiveGap = 200; // unused receive addresses checked past the last used one
    int utxoChangeGap = 50;
    int evmGap = 100;
    int evmRefreshSeconds = 60;
    // Per NetworkInfo::id, in the user's order; none or empty: the network's built-in list.
    QMap<QString, QStringList> servers;

    bool operator==(const ScanSettings&) const = default;
};

} // namespace wallet
