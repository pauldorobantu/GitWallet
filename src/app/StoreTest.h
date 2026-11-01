#pragma once

#include <QString>

namespace app {

struct StoreTestResult {
    int passed = 0;
    int total = 0;
    QString report; // one line per check
};

// Checks what many wallets rest on, offline and without a window: a first-format
// wallet.dat loading as one wallet and saved in the current format, ids that are
// never reused, repeated keys refused, public keys parsed and refused correctly,
// and per-wallet totals and counts in the address model. Works on the current
// data folder and only the public BIP39 test mnemonics, so main() runs it on a
// separate --data-dir alone.
StoreTestResult runStoreTest();

} // namespace app
