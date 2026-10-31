#pragma once

#include "chain/Bip32.h"
#include "chain/Hash.h"

#include <string>

// Turns keys into addresses per chain / script type, and produces the lookup
// keys the network layer needs (the Electrum scripthash for Bitcoin).
namespace chain {

enum class ScriptType {
    P2PKH,       // BTC legacy          m/44'/0'
    P2SH_P2WPKH, // BTC nested segwit   m/49'/0'
    P2WPKH,      // BTC native segwit   m/84'/0'
    EthAccount,  // Ethereum            m/44'/60'
};

const char* scriptTypeName(ScriptType t);
bool isBitcoin(ScriptType t);

// The extended-key version an account of this type is shown with
// (zpub for BIP84, ypub for BIP49, xpub otherwise).
uint32_t xpubVersionFor(ScriptType t);

struct AddressInfo {
    std::string address;            // what the user sees / shares
    Bytes scriptPubKey;             // BTC: the output script (empty for ETH)
    std::string electrumScripthash; // BTC: sha256(spk) reversed, hex (empty for ETH)
    std::string path;               // the full derivation path used
};

// m/purpose'/coin'/account'  (the account node GitWallet stores as an xpub).
std::string accountRootPath(ScriptType t, uint32_t account);
// m/purpose'/coin'/account'/change/index
std::string accountPath(ScriptType t, uint32_t account, uint32_t change, uint32_t index);

// Encodes one public key as an address of type `t`. `compressedPub` (33 bytes)
// is used for Bitcoin; `uncompressedPub` (65 bytes) for Ethereum. `path` is left
// empty.
AddressInfo addressFromPublicKey(ScriptType t, const Bytes& compressedPub,
                                 const Bytes& uncompressedPub);

// Private derivation from the master node (used at import and in self-tests).
AddressInfo addressAt(const HdKey& master, ScriptType t,
                      uint32_t account, uint32_t change, uint32_t index);

// Public (watch-only) derivation from the stored account node. For long scans
// derive the chain node once (account.deriveChild(change)) and call
// addressFromChainNode for each index: one CKDpub per address instead of two.
AddressInfo addressFromAccount(const HdPubKey& accountNode, ScriptType t,
                               uint32_t account, uint32_t change, uint32_t index);
AddressInfo addressFromChainNode(const HdPubKey& chainNode, ScriptType t,
                                 uint32_t account, uint32_t change, uint32_t index);

// EIP-55 mixed-case checksum for a 20-byte Ethereum address (adds "0x").
std::string ethChecksumAddress(const Bytes& addr20);

} // namespace chain
