#pragma once

#include "chain/Hash.h"

#include <string>
#include <vector>

// BIP39: mnemonic sentence <-> seed. The English wordlist is loaded once from
// the embedded resource (:/bip39/english.txt). Mnemonic words are ASCII, so no
// Unicode normalisation is needed on them; a passphrase, if any, must be passed
// already NFKD-normalised (the GUI does this with Qt).
namespace chain::bip39 {

// Splits a sentence into lowercase words on whitespace.
std::vector<std::string> split(const std::string& sentence);

// True if every word is in the list and the BIP39 checksum matches.
// Accepts 12/15/18/21/24-word mnemonics.
bool validate(const std::vector<std::string>& words, std::string* errorOut = nullptr);

// Mnemonic -> 64-byte seed (PBKDF2-HMAC-SHA512, 2048 rounds, salt
// "mnemonic"+passphrase). Does not validate the checksum; call validate first.
Bytes toSeed(const std::vector<std::string>& words, const std::string& passphrase = {});

} // namespace chain::bip39
