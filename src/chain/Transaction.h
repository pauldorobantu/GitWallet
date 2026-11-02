#pragma once

#include "chain/Address.h"
#include "chain/Hash.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Reading what a server hands over: a raw transaction and a block header are bytes that
// hash to a name, and neither is taken for anything until they do. Everything here keeps
// hashes in the order explorers write them, the reverse of the order they are computed in.
namespace chain {

struct Hash256 : std::array<uint8_t, 32> {};

std::string hashHex(const Hash256& hash);
bool hashFromHex(const std::string& hex, Hash256* out); // 64 hex digits, or false

// One output of one transaction.
struct OutPoint {
    Hash256 txid{};
    uint32_t n = 0;
    bool operator==(const OutPoint& other) const = default;
};

struct TxInput {
    OutPoint previous;
    uint32_t sequence = 0;
};

struct TxOutput {
    uint64_t value = 0;
    Bytes script;
};

struct Transaction {
    Hash256 txid{};
    std::vector<TxInput> inputs;
    std::vector<TxOutput> outputs;
    int size = 0;         // bytes as sent, witness included
    int strippedSize = 0; // bytes that hash to txid
    int vsize = 0;        // what a fee rate is per. Witness bytes count, and no id covers those: a server's word

    bool coinbase() const; // its one input spends nothing
    bool signalsReplaceable() const; // BIP125
    uint64_t outputTotal() const;
};

// One whole transaction, with or without witness data, and not a byte more, that a block could
// hold: no heavier than one, and spending no output twice. False on anything else.
bool parseTransaction(const Bytes& raw, Transaction* out);

// What an output's script pays to, as the user reads it. Empty when it has no address (a
// bare public key, a data carrier, anything else).
std::string addressFromScript(const Bytes& script, const AddressFormat& format);
// What a server knows it by: sha256(script), reversed, in hex.
std::string scripthashOfScript(const Bytes& script);

struct BlockHeader {
    Hash256 hash{};
    Hash256 previous{};
    Hash256 merkleRoot{};
    int32_t version = 0;
    uint32_t time = 0;
    uint32_t bits = 0;
    uint32_t nonce = 0;
};

bool parseHeader(const Bytes& raw, BlockHeader* out); // exactly 80 bytes, or false
Bytes serializeHeader(const BlockHeader& header);     // those 80 bytes again

// The header's own hash is at or under the target its bits ask for, and those are at or
// under `limit`: the work was done, at a difficulty the chain allows.
bool headerHasWork(const BlockHeader& header, uint32_t limit);

// What the bits of the first block of a period must be, from the last header of the period
// before and the time of its first one (Bitcoin's rule, off by one as it has always been).
uint32_t nextBits(uint32_t lastBits, uint32_t firstTime, uint32_t lastTime, uint32_t limit, uint32_t targetSeconds);

// The root a Merkle branch leads to from `leaf`, at position `position` among the leaves:
// a transaction among those of a block, or a header among those of a checkpoint.
Hash256 merkleRootOf(const Hash256& leaf, const std::vector<Hash256>& branch, uint64_t position);

// Whether a branch proves a transaction in a block with this root. The two hashes under a node of
// the tree are 64 bytes, and those of a real 64-byte transaction are a node to anyone who only
// hashes them: what looks like its half is then "proven" by a branch through it (CVE-2017-12842).
// A branch through what reads as a transaction proves nothing, as in Electrum.
bool proofLeadsTo(const Hash256& leaf, const std::vector<Hash256>& branch, uint64_t position, const Hash256& root);

} // namespace chain
