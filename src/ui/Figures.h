#pragma once

#include "chain/Amount.h"
#include "wallet/Types.h"

#include <QString>

// How amounts read on the Overview and Wallets pages.
namespace app::ui::figures {

QString dash();

// Amount::format() does the exact conversion (truncated, trailing zeros
// trimmed); on top of it the whole part gets thousands separators and the
// dollar tokens keep two decimals: 1234567.5 USDT reads "1,234,567.50".
// Throws whatever Amount::format() throws.
QString formatAmount(const chain::Amount& amount, const wallet::AssetInfo& info);

QString formatUsd(double value);
QString formatCount(int n);

// What is known so far, as the lower bound it is: "≥ 0.5", "≥ $1,234.00".
// Dust that only reads "<0.000001" or "<$0.01" becomes "> 0" / "> $0".
QString atLeast(const QString& figure);

// What counts as dust, coin by coin: "0.00001 BTC, 0.01 USDT".
QString dustLimits();

// Whether the lists leave dust out, as the reader last chose. They do until told otherwise.
bool hideDust();
void setHideDust(bool hide);

// A count of what has answered so far: plain once all of it has, "≥ 3" before that, "—" for none yet.
QString countSoFar(int count, bool complete);

} // namespace app::ui::figures
