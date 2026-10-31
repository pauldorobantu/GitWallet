#pragma once

#include <string>

// Offline correctness checks against published test vectors (BIP32, BIP84,
// and the standard Ethereum test mnemonic). Run at startup and from
// `GitWallet --selftest`; if these fail the derivation engine is wrong and no
// address it produces can be trusted.
namespace chain {

struct SelfTestResult {
    bool ok = false;
    int passed = 0;
    int total = 0;
    std::string report; // one line per check
};

SelfTestResult runSelfTest();

} // namespace chain
