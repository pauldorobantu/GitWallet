#include "chain/SelfTest.h"

#include "chain/Address.h"
#include "chain/Bip32.h"
#include "chain/Bip39.h"
#include "chain/Hash.h"

namespace chain {

namespace {

const char* kTrezorMnemonic =
    "abandon abandon abandon abandon abandon abandon "
    "abandon abandon abandon abandon abandon about";

} // namespace

SelfTestResult runSelfTest()
{
    SelfTestResult r;
    auto check = [&](const char* name, const std::string& got, const std::string& want) {
        ++r.total;
        const bool ok = (got == want);
        if (ok) ++r.passed;
        r.report += std::string(ok ? "[ok]  " : "[FAIL] ") + name + "\n";
        if (!ok) r.report += "        got : " + got + "\n        want: " + want + "\n";
    };

    // --- BIP39: the canonical "abandon ... about" mnemonic -----------------
    {
        const auto words = bip39::split(kTrezorMnemonic);
        std::string err;
        check("bip39 validate (valid)", bip39::validate(words, &err) ? "true" : ("false:" + err),
              "true");

        // Tamper one word -> checksum must fail.
        auto bad = words;
        bad.back() = "zoo";
        check("bip39 validate (tampered)", bip39::validate(bad) ? "true" : "false", "false");

        // Seed with empty passphrase (well-known vector).
        check("bip39 seed (no passphrase)", toHex(bip39::toSeed(words)),
              "5eb00bbddcf069084889a8ab9155568165f5c453ccb85e70811aaed6f6da5fc1"
              "9a5ac40b389cd370d086206dec8aa6c43daea6690f20ad3d8d48b2d2ce9e38e4");

        // Seed with passphrase "TREZOR" (BIP39 reference vector).
        check("bip39 seed (TREZOR)", toHex(bip39::toSeed(words, "TREZOR")),
              "c55257c360c07c72029aebc1b53c05ed0362ada38ead3e3e9efa3708e5349553"
              "1f09a6987599d18264c1e1c92f2cf141630c7a3c4ab7c81b2f001698e7463b04");
    }

    // --- BIP32 test vector 1 (seed 000102...0f) -----------------------------
    {
        const Bytes seed = fromHex("000102030405060708090a0b0c0d0e0f");
        const HdKey m = HdKey::fromSeed(seed);
        check("bip32 m public key", toHex(m.publicKeyCompressed()),
              "0339a36013301597daef41fbe593a02cc513d0b55527ec2df1050e2e8ff49c85c2");
        const HdKey m0h = m.deriveChild(0, true); // m/0'
        check("bip32 m/0' public key", toHex(m0h.publicKeyCompressed()),
              "035a784662a4a20a65bf6aab9ae98a6c068a81c52e4b032c0fb5400c706cfccc56");
    }

    // --- BIP84 native-segwit vectors (from the BIP84 spec) ------------------
    {
        const HdKey m = HdKey::fromSeed(bip39::toSeed(bip39::split(kTrezorMnemonic)));
        check("bip84 m/84'/0'/0'/0/0",
              addressAt(m, ScriptType::P2WPKH, 0, 0, 0).address,
              "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu");
        check("bip84 m/84'/0'/0'/0/1",
              addressAt(m, ScriptType::P2WPKH, 0, 0, 1).address,
              "bc1qnjg0jd8228aq7egyzacy8cys3knf9xvrerkf9g");
        check("bip84 m/84'/0'/0'/1/0 (change)",
              addressAt(m, ScriptType::P2WPKH, 0, 1, 0).address,
              "bc1q8c6fshw2dlwun7ekn9qwf37cu2rn755upcp6el");
    }

    // --- Ethereum (standard test mnemonic, m/44'/60'/0'/0/0) ----------------
    {
        const HdKey m = HdKey::fromSeed(bip39::toSeed(bip39::split(kTrezorMnemonic)));
        check("eth m/44'/60'/0'/0/0",
              addressAt(m, ScriptType::EthAccount, 0, 0, 0).address,
              "0x9858EfFD232B4033E47d90003D41EC34EcaEda94");
    }

    r.ok = (r.passed == r.total);
    return r;
}

} // namespace chain
