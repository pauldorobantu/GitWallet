# GitWallet

A desktop wallet for Windows that shows **every address derived from one seed**
— across Bitcoin and Ethereum — with per-address balances and one combined
total in BTC, ETH and USD. Built for seeds that generate many addresses (e.g. a
shop that makes a fresh address per order), where ordinary wallets stop scanning
after the first gap and miss funds further down.

> **Status: stage 1 (view-only) works and is verified** — see *Verification*
> below. Spending (stage 2: BTC, stage 3: ETH and tokens) is added next and is
> tested on test networks before it touches real funds.

![Overview](docs/screenshot-overview.png)

![Addresses](docs/screenshot-addresses.png)

## What it does

1. **View** *(done)* — import a BIP39 seed once; GitWallet derives the account
   public keys and scans every Bitcoin address type (native SegWit, nested
   SegWit, legacy — receive and change) and Ethereum (ETH, USDT, USDC) far past
   the last used address (gap limits are configurable, default 200 / 50 / 100).
   Each address shows its balance and state; the Overview shows the totals in
   BTC, ETH, USDT, USDC and USD. Addresses another app creates later from the
   same seed appear by themselves (live Electrum subscriptions, periodic
   Ethereum refresh).
2. **Send BTC** *(next)* — sweep selected or all addresses into one transaction.
3. **Sweep ETH + tokens** *(after that)* — empty each account in turn (Ethereum
   needs one transaction per address; gas is funded automatically for tokens).

## What is stored

Only the four **account-level public keys** (zpub / ypub / xpub at
`m/84'/0'/0'`, `m/49'/0'/0'`, `m/44'/0'/0'`, `m/44'/60'/0'`) and the scan
settings, in `wallet.dat`, encrypted for the current Windows user with DPAPI.
The seed is used once at import to derive those keys and is then scrubbed from
memory; it is never written anywhere. Public keys can show balances but cannot
spend.

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

- **BIP39 / BIP32 / BIP44 / BIP49 / BIP84** derivation, watch-only from the
  stored account keys.
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
| `--walkthrough`: the real UI driven like a user — import with three kinds of mistakes, import, scan, search, filter, settings, close and reopen from `wallet.dat`, remove | 17 steps, 34 / 34 checks |

Test-only switches (they refuse to run without `--data-dir`, so they never touch
the real wallet):

```bat
GitWallet --selftest
GitWallet --data-dir C:\tmp\gw --import-test-mnemonic --scan-report report.json
GitWallet --data-dir C:\tmp\gw2 --walkthrough C:\tmp\walk
```

## Build

Requirements: Visual Studio 2022 (MSVC x64), Qt 6.10.3 at
`C:\Qt\6.10.3\msvc2022_64`, CMake + Ninja, and vcpkg at `C:\app-vcpkg`.

```bat
build.bat
```

This configures with the vcpkg toolchain (Botan, zlib, secp256k1) and produces
`build\GitWallet.exe`.

## Security

- The seed never leaves the machine and is never written to disk.
- Servers only ever see addresses (Bitcoin scripthashes, Ethereum addresses).
- TLS / HTTPS certificate verification is always on.
- Spending features will be tested on Bitcoin testnet and Ethereum Sepolia
  before release; real transfers are always initiated by you.

## License

MIT — see [LICENSE](LICENSE). Icons: [Lucide](https://lucide.dev) (ISC).
