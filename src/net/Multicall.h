#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

// Minimal ABI codec for Multicall3, deployed at
// 0xcA11bde05977b3631167028862bE2a173976CA11 on Ethereum mainnet: one eth_call
// returns the ETH balance and the ERC-20 balances of many addresses at once.
namespace net::multicall {

inline const char* const kAddress = "0xcA11bde05977b3631167028862bE2a173976CA11";

struct Call {
    QString target;      // "0x…" contract address
    QByteArray callData; // raw calldata (selector + ABI-encoded arguments)
};

struct Result {
    bool success = false;
    QByteArray returnData; // raw bytes (for balances: one 32-byte uint256 word)
};

// aggregate3((address target, bool allowFailure, bytes callData)[] calls)
// with allowFailure = true for every call; returns the complete calldata.
QByteArray encodeAggregate3(const QList<Call>& calls);
// Decodes aggregate3's return value, (bool success, bytes returnData)[].
// False if the data is malformed.
bool decodeAggregate3(const QByteArray& returnData, QList<Result>* out);

QByteArray encodeGetEthBalance(const QString& address); // Multicall3.getEthBalance(address)
QByteArray encodeBalanceOf(const QString& address);     // ERC-20 balanceOf(address)

QString toHex(const QByteArray& bytes);            // "0x" + lowercase hex
bool fromHex(const QString& hex, QByteArray* out); // accepts an optional "0x"

} // namespace net::multicall
