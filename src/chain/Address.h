#pragma once

#include "chain/Bip32.h"
#include "chain/Hash.h"

#include <string>

// Turns keys into addresses per chain / script type, and produces the lookup
// keys the network layer needs (the Electrum scripthash for UTXO chains).
namespace chain {

enum class ScriptType {
    P2PKH,       // legacy          m/44'/coin'
    P2SH_P2WPKH, // nested segwit   m/49'/coin'
    P2WPKH,      // native segwit   m/84'/coin'
    EthAccount,  // account         m/44'/60'
};

const char* scriptTypeName(ScriptType t);
// The name wallet.dat and the list of coins call it by.
const char* scriptTypeId(ScriptType t);
bool scriptTypeFromId(const std::string& id, ScriptType* out);
// Its addresses hold coins, as opposed to an account's balances.
bool isUtxo(ScriptType t);

// What a network's addresses look like: its BIP44 coin type and, for a UTXO network,
// its address prefixes.
struct AddressFormat {
    uint32_t coinType = 0;
    uint8_t p2pkhVersion = 0x00;
    uint8_t p2shVersion = 0x05;
    std::string hrp = "bc";
};

// Bitcoin's for a UTXO type, Ethereum's for an account: what a call without a format means.
const AddressFormat& defaultFormat(ScriptType t);

// The extended-key version an account of this type is shown with
// (zpub for BIP84, ypub for BIP49, xpub otherwise).
uint32_t xpubVersionFor(ScriptType t);

struct AddressInfo {
    std::string address;            // what the user sees / shares
    Bytes scriptPubKey;             // UTXO: the output script (empty for an account)
    std::string electrumScripthash; // UTXO: sha256(spk) reversed, hex (empty for an account)
    std::string path;               // the full derivation path used
};

// m/purpose'/coin'/account'  (the account node GitWallet stores as an xpub).
std::string accountRootPath(ScriptType t, uint32_t account, const AddressFormat& format);
std::string accountRootPath(ScriptType t, uint32_t account);
// m/purpose'/coin'/account'/change/index
std::string accountPath(ScriptType t, uint32_t account, uint32_t change, uint32_t index,
                        const AddressFormat& format);
std::string accountPath(ScriptType t, uint32_t account, uint32_t change, uint32_t index);

// Encodes one public key as an address of type `t`. `compressedPub` (33 bytes)
// is used for a UTXO type; `uncompressedPub` (65 bytes) for an account. `path` is
// left empty.
AddressInfo addressFromPublicKey(ScriptType t, const Bytes& compressedPub,
                                 const Bytes& uncompressedPub, const AddressFormat& format);
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
                                 uint32_t account, uint32_t change, uint32_t index,
                                 const AddressFormat& format);
AddressInfo addressFromChainNode(const HdPubKey& chainNode, ScriptType t,
                                 uint32_t account, uint32_t change, uint32_t index);

// EIP-55 mixed-case checksum for a 20-byte Ethereum address (adds "0x").
std::string ethChecksumAddress(const Bytes& addr20);

} // namespace chain
