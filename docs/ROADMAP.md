# GitWallet roadmap

Written 2026-11-01, from a study of Exodus, Trust Wallet, MetaMask, Ledger Live and
Electrum (their help pages and code, not hands-on use) and a critical pass over the
first draft. Limits of outside services change often: treat the figures as dated.

## What it has to become

- Nearly all coins. Each coin on its own and everything together, with a total balance.
- Many seeds and many addresses on top of that, which none of the five does.
- USD on the grand total, per seed and per address; per address, each asset in its own coin.
- Every transaction visible in a history, overall and on each address.
- The same logic as the wallets people already know, so it stays easy to use.

## Where it stands

USD at every level and per-address amounts exist, for four assets only: BTC, ETH, USDT,
USDC. Missing against the known wallets: History, transaction detail, Receive with QR,
a per-coin screen, address labels, explorer links, app password, Send. Nothing keeps
transactions today, and every start scans again.

## The logic people already know

All five: one total, then a list you open downward; every transaction opens to detail
and an explorer link; pending looks different from confirmed; Receive shows an address
and a QR; Send is one path (asset, recipient, amount, fee, confirm); detail stays hidden
until asked for. GitWallet takes the first screen from Exodus, the address tables from
Electrum, the transaction detail and CSV from Ledger, search and fee presets from MetaMask.

With many seeds it opens on a Portfolio (grand total, one row per coin) and reaches the
same screens two ways, coin -> seeds -> addresses -> transaction and seed -> coins ->
addresses -> transaction, with a clickable path along the top and a History tab at every
level. An address opens to every asset in its own coin with USD, then its history.

## Honest limits

- **"Nearly all" means about 50 chains and their listed tokens**, as Exodus does, with
  more on real need. The big coin counts of other wallets are mostly tokens. Every token
  on every chain on every address is not possible with no server and no key.
- **Bitcoin family history needs nothing else.** Ethereum-style native transfers cannot be
  listed by a plain node: they need an indexer, so a free key the owner pastes (never one
  built into a public app). Token transfers come from logs, within limits still to measure.
- **Only public keys are stored**, which is right and has a price: a chain that uses other
  keys appears for an existing seed only after typing that seed once more. The list of
  chains, accounts and paths to capture is written first, so that happens once.
- **Solana, TON and similar chains** cannot make new addresses from public keys: a fixed
  batch is derived while the seed is there.
- **Scale.** 500 wallets at the default gaps are 425,000 addresses. One connection follows
  40,000 Bitcoin addresses live, so the next unused addresses and anything issued from
  Receive stay live, and the rest are read in slower passes. Detecting a payment to a fresh
  address must not get slower than today; the worst delay per chain is shown.
- **Public servers see the addresses asked about**, and a key ties them to whoever holds it.
  Answers are checked (Merkle proofs against headers for Bitcoin, a second endpoint for
  Ethereum-style chains) before "paid" is believed.
- **Many chains will be view-only for a long time.** Sending beyond Bitcoin and
  Ethereum-style chains is extra work per family, and the app says so per chain.

## Stages

Each ends with a report and waits for the owner. Sizes are rough, in sessions of about an
hour, reviews included. Each is verified against independent sources, and CI drives the
real UI.

| # | Stage | What the owner gets | Size |
|---|---|---|---|
| 1 | **Foundation** | Coins and chains become data, an encrypted local database and one scheduler. Starts instantly from saved balances. Live price source proven, with the time shown beside USD. Real limits of public endpoints measured | 3-5 |
| 2 | **Bitcoin history** | Every BTC transaction, overall and per address; detail with fee, USD then and now, explorer link; pending, replaced, confirmed; filters, search, CSV, labels and notes. No key needed | 4-6 |
| 3 | **Ethereum history** | The same for ETH, USDT, USDC, with failed transactions. "Complete back to" shown per chain | 3-5 |
| 4 | **Familiar screens** | Portfolio, coin pages, clickable path, Receive with QR and next unused address, hide spam and zeros, payment alerts, app password, portable backup | 6-8 |
| 5 | **Ethereum-style chains** | BNB, Base, Arbitrum, Polygon, OP, Avalanche, custom tokens. No retyping | 3-5 |
| 6 | **Tron and Bitcoin forks** | USDT on Tron, Litecoin, Dogecoin, Bitcoin Cash, Dash. Seeds typed once more, after the capture list is verified per chain | 6-10 |
| 7 | **Send** | Normal send and sweep for Bitcoin and Ethereum-style chains: fee presets, coin control, address book, first-time recipient warnings. Test networks first, then a small real payment the owner approves | 12-18 |
| 8 | **More families** | XRP, Solana, then Cosmos, Cardano, TON and others, one at a time, each counted only when balances, history and prices pass, up to about 50 chains | 25-40 |

Coins and send together are likely above 100 sessions. Every stage leaves something usable.

## Decisions only the owner can make

1. **API keys he pastes, optional.** Recommended yes: without them Ethereum and Tron history
   stays incomplete. Needs sign-ups at a few providers, and the keys stay in the encrypted file.
2. **About 50 chains plus listed tokens as "nearly all".** Recommended yes.
3. **More coins before send.** Recommended yes, unless paying out comes first in practice.
4. **Which wallets created his seeds.** Decides the paths and accounts to capture, including
   Taproot and second accounts, which are not scanned today.
5. **How to sign, before Send.** Typing seeds on an online PC for every send is neither usable
   nor safe. Recommended: the same app on an offline PC signing files the online one prepares.
6. **Code signing before any stage that takes real seeds.** Recommended yes, on his firm.
