# GitWallet

A desktop wallet for Windows for businesses that take many payments on unique
addresses: **many seeds, every address of each one**, across Bitcoin and
Ethereum, with per-address balances, a total per wallet and one combined total
in BTC, ETH and USD. Built for seeds that generate many addresses (e.g. a shop
that makes a fresh address per order), where ordinary wallets stop scanning
after the first gap and miss funds further down.

> **Status: stage 1 (view-only) works.** The single-seed view is verified — see
> *Verification* below. Spending (stage 2: BTC, stage 3: ETH and tokens) is added
> next and is tested on test networks before it touches real funds.

![Overview](docs/screenshot-overview.png)

![Addresses](docs/screenshot-addresses.png)

## What it does

1. **View** *(done)* — add as many named wallets as you need, up to 500: a BIP39
   seed once, or just its public keys (zpub, ypub, xpub) when the seed should
   never touch this PC. **One at a time or a whole list:** *Add many* takes one seed
   or one set of public keys per line, pasted or from a text or CSV file, each with
   a name if you want (`Shop 1: word word ...`, `"Shop 2","word word ..."`, a
   numbered list). The good lines go in together; what did not go in stays in the
   field, each line named by its number with the reason and never quoted. A seed
   already here, a name in use and a line that can be read two ways are refused,
   and seed words or a private key are never taken as a name. A line of public keys
   can hold a whole wallet: its second xpub is read as the Ethereum account. GitWallet
   derives the account public keys and scans every
   Bitcoin address type (native SegWit, nested SegWit, legacy — receive and
   change) and Ethereum (ETH, USDT, USDC) far past the last used address (gap
   limits are configurable, default 200 / 50 / 100). The Wallets page lists what
   each wallet holds; each address shows its wallet, balance and state, can be
   filtered by wallet, chain and state, and what the filters leave exports to
   CSV for the books; the Overview shows the totals in BTC, ETH, USDT, USDC and
   USD. Addresses another app creates later from the same seed appear by
   themselves (live Electrum subscriptions, periodic Ethereum refresh).
2. **Send BTC** *(next)* — sweep selected or all addresses into one transaction.
3. **Sweep ETH + tokens** *(after that)* — empty each account in turn (Ethereum
   needs one transaction per address; gas is funded automatically for tokens).

All wallets share one Electrum connection, which follows at most 40,000 Bitcoin
addresses in total: public servers refuse more per client. At the default gaps
that is about 50 wallets before any is used. GitWallet says so when it stops
there, and every wallet then reads as not whole; lower the gaps in Settings or
remove wallets you do not need here. Ethereum stops at 200,000 addresses the same
way.

A plain `xpub` does not say what it is, because wallets write SegWit accounts with
that prefix too, so GitWallet asks and shows the first address of each key to
compare with the other wallet.

## What is stored

Only each wallet's name and **account-level public keys** (zpub / ypub / xpub at
`m/84'/0'/0'`, `m/49'/0'/0'`, `m/44'/0'/0'`, `m/44'/60'/0'`, or those you pasted)
and the scan settings, in `wallet.dat`, encrypted for the current Windows user
with DPAPI. A seed is used once to derive those keys and is then scrubbed from
memory; it is never written anywhere. Public keys can show balances but cannot
spend. A `wallet.dat` from an earlier version loads as one wallet. Starting
GitWallet again brings the open window to the front instead of opening a second
one, and only one window at a time writes `wallet.dat`. A CSV export is not
encrypted: anyone who reads it sees those addresses and balances.

If `wallet.dat` cannot be read (another Windows user's, damaged, from a newer
GitWallet), it is never overwritten: adding a wallet keeps it beside the new file as
`wallet.dat.unread-<date>-<time>` and says so. "Remove all wallets" deletes those
too.

## Network

- **Bitcoin:** Electrum protocol over TLS to public servers whose certificates
  verify against the Windows CA store (self-signed servers are not used); one
  server at a time, automatic failover, live subscriptions.
- **Ethereum:** public JSON-RPC endpoints (no API key), one Multicall3
  `eth_call` per 50 addresses for ETH + USDT + USDC balances, failover between
  endpoints.
- **Prices:** CoinGecko's public API.

Servers and endpoints can be replaced in Settings.

## Design

The window, title bar, sidebar, tables and theming are the **FlowSuite v3
design system** (C++/Qt 6 Widgets), reused here so GitWallet matches the rest of
the suite. Only the look-and-feel layer was reused; all wallet logic is new
(`src/chain`, `src/net`, `src/wallet`, `src/app`, `src/ui/pages`).

## Cryptography

- **BIP39 / BIP32 / BIP44 / BIP49 / BIP84** derivation, watch-only from each
  wallet's stored account keys.
- [libsecp256k1](https://github.com/bitcoin-core/secp256k1) (Bitcoin Core's
  library) for elliptic-curve operations.
- [Botan](https://botan.randombit.net/) for SHA-256/512, RIPEMD-160,
  HMAC-SHA512, PBKDF2, Keccak-256 and big-integer amounts.

## Verification

Every claim below is reproducible with the public BIP39 test mnemonic
(`abandon … about`) in a throw-away data folder:

| Check | Result |
|---|---|
| `GitWallet --selftest` — BIP39, BIP32 (TV1 incl. xpubs), BIP84 (zpub + addresses), public == private derivation, Ethereum, amounts | 21 / 21 |
| Bitcoin addresses vs **Electrum 4.8.3** watch-only wallets from the same zpub / ypub / xpub (receive + change, all three types) | 90 / 90 identical |
| Every Bitcoin address's history and balance vs **Blockstream's electrs** server (a different operator and implementation than the scan used) + a mempool.space sample | 988 / 988 identical |
| Every Ethereum address's nonce, ETH, USDT and USDC vs a **different RPC node**, with plain calls instead of Multicall | 297 / 297 identical |
| `--walkthrough`: the real UI driven like a user — a seed with three kinds of mistakes, a second seed, the same seed refused, public keys, totals per wallet adding up, filter, CSV, rename, settings, close and reopen from `wallet.dat`, remove one, a list of seeds and of public keys from a file and a paste, remove all | run on every push (see below) |
| `--store-test`: a `wallet.dat` of the earlier format, ids, repeated keys, public-key parsing, reading lists, adding in a batch and per-wallet totals, offline | run on every push (see below) |

The address and balance comparisons above were made on the single-seed view; the
address derivation they cover is unchanged. Every push builds GitWallet on a clean
Windows machine and runs the self-test, the store test and the walkthrough there
(`.github/workflows/build.yml`), since Windows Defender may quarantine an unsigned
local build.

Test-only switches (they refuse to run without `--data-dir`, so they never touch
the real wallet):

```bat
GitWallet --selftest
GitWallet --data-dir C:\tmp\gw --import-test-mnemonic --scan-report report.json
GitWallet --data-dir C:\tmp\gw2 --walkthrough C:\tmp\walk
GitWallet --data-dir C:\tmp\gw3 --store-test
GitWallet --data-dir C:\tmp\gw4 --demo-wallets
```

`--demo-wallets` fills the folder with published test seeds, to see the app with real
addresses and balances. Anyone can spend from those seeds: never send anything to them.

## Build

Requirements: Visual Studio 2022 (MSVC x64), Qt 6.10.3 at
`C:\Qt\6.10.3\msvc2022_64`, CMake + Ninja, and vcpkg at `C:\app-vcpkg`.

```bat
build.bat
```

This configures with the vcpkg toolchain (Botan, zlib, secp256k1), builds
`build\GitWallet.exe` with Qt beside it and deploys a copy to `dist\` (the exe, Qt
and the Visual C++ runtime, with the Qt plugins in `dist\lib\plugins`). Start
`dist\GitWallet.exe`; the `dist` folder can be copied anywhere. Builds are not signed
yet, so a PC with Smart App Control on blocks them, and Windows Defender may flag a
new build.

## Security

- A seed never leaves the machine and is never written to disk.
- Servers only ever see addresses (Bitcoin scripthashes, Ethereum addresses).
- TLS / HTTPS certificate verification is always on.
- Spending features will be tested on Bitcoin testnet and Ethereum Sepolia
  before release; real transfers are always initiated by you.

## License

MIT — see [LICENSE](LICENSE). Icons: [Lucide](https://lucide.dev) (ISC).
