#pragma once

#include "chain/Bip32.h"
#include "chain/Hash.h"

#include <string>

// Turns derived keys into addresses per chain / script type, and produces the
// lookup keys the network layer needs (Electrum scripthash for Bitcoin).
namespace chain {

enum class ScriptType {
    P2PKH,       // BTC legacy          m/44'/0'
    P2SH_P2WPKH, // BTC nested segwit   m/49'/0'
    P2WPKH,      // BTC native segwit   m/84'/0'
    EthAccount,  // Ethereum            m/44'/60'
};

const char* scriptTypeName(ScriptType t);

struct AddressInfo {
    std::string address;            // what the user sees / shares
    Bytes scriptPubKey;             // BTC: the output script (empty for ETH)
    std::string electrumScripthash; // BTC: sha256(spk) reversed, hex (empty for ETH)
    std::string path;               // the full derivation path used
};

// Standard account path for a type: m/purpose'/coin'/account'/change/index.
std::string accountPath(ScriptType t, uint32_t account, uint32_t change, uint32_t index);

// Derive one address from the master node.
AddressInfo addressAt(const HdKey& master, ScriptType t,
                      uint32_t account, uint32_t change, uint32_t index);

// EIP-55 mixed-case checksum for a 20-byte Ethereum address (adds "0x").
std::string ethChecksumAddress(const Bytes& addr20);

} // namespace chain
