# What public servers allow

Measured on 2026-11-02 from one home connection, with no key, a few thousand requests per
server at most. Limits like these change without notice, and the same server may treat
another address differently: the numbers say what to design for, not what is promised.
`res/coins.json` keeps the pace of each built-in endpoint, and every client still expects
to be refused: an address an Electrum server will not follow is read, reads stop for a
while after a refusal, and an endpoint that just failed gets no new work while another is left.

## Prices

| Source | Answer | Limits seen | Used for |
|---|---|---|---|
| **DefiLlama** `coins.llama.fi/prices/current` | By `chain:contract` or `coingecko:id`, with price, timestamp, confidence, symbol, decimals. Cached by a CDN for seconds | 5 calls 3 s apart all fine. A URL over 9,000 characters refused (HTTP 414), about 150 assets a call | First source |
| **CoinGecko** `simple/price` | By its id, with `last_updated_at` | 3 calls in 9 s, then HTTP 429 with `Retry-After: 60` | Second, for what the first does not answer |
| **Coinbase** `v2/exchange-rates` | 662 symbols in one call, as units per dollar | 5 calls 3 s apart all fine. By symbol only: TRX was missing, and a symbol can mean another coin | Last, only for an asset the list names |
| Kraken `Ticker`, Bitstamp | By trading pair | Fine, a few calls | Not used; agree with the others to about 0.1% |
| Binance `ticker/price` | Against USDT, not USD | Fine from here; known to block some countries | Not used |
| CryptoCompare | | HTTP 401: a key is required now | Not used |

All sources agreed to within about 0.15% on BTC, ETH, USDT and USDC at the same moment
(USDT 0.9986 to 0.9990).

**USD on a past day:** DefiLlama `prices/historical/{time}/{coins}` answered 2023 for all four
assets within two minutes of the time asked, and 2015 for Bitcoin only, and `chart` gives 30
daily points in one call. CoinGecko without a key refuses anything over 365 days old (HTTP
401). Kraken `OHLC` gives 720 daily candles.

`batchHistorical` takes many moments of one coin in one call: 500 fine, 1,000 over the URL limit
(HTTP 414), ten calls of 200 in 3.5 s all answered. The answer lists a point per moment it has,
closest first and leaving out those it has none for, so an answer has to be matched by its time: within
minutes in recent years, the same day before about 2020, nothing before 2015.

## Bitcoin: Electrum servers

| Server | Software | 1,000 `get_history`, 64 in flight | 2,000 subscribes | 30,000 `get_history` at 250/s | `unsubscribe` |
|---|---|---|---|---|---|
| electrum.jhoenicke.de:50002 | Fulcrum 2.1.2 | 1,508/s | 1,597/s | No error, 40 ms | Yes |
| e2.keff.org:50002 | Fulcrum 2.1.3 | 768/s | 871/s | No error, 67 ms | Yes |
| e.keff.org:50002 | Fulcrum 2.1.3 | 961/s | 942/s | | Yes |
| fulcrum.grey.pw:51002 | Fulcrum 2.1.4 | 1,090/s | 1,176/s | No error, 54 ms | Yes |
| electrum.hodlister.co:50002 | ElectrumX 1.10.0 | 401/s | 379/s | No error, 133 ms | **No** |
| electrum.blockstream.info:50002 | electrs-esplora 0.4.1 | 311/s | 328/s | **Stopped at 22,124**: asks for a pause of 147 s, then closes | Yes |
| blackie.c3-soft.com:57002 | Fulcrum 2.1.4 | 329/s | 325/s | | Yes |
| blockstream.info:700 | electrs-esplora 0.4.1 | 328/s | 331/s | | Yes |
| electrum.diynodes.com:50022 | | Closed the connection at once | | | |

The last one is off the list. All the others give 2,016 headers a call, raw transactions and
Merkle proofs by height. `verbose` transactions are refused by electrs, so raw ones have to be
parsed.

**Proving a header (2026-11-02):** all eight answered `blockchain.block.header(height, checkpoint)`
with a Merkle branch of 20 steps, and all eight gave the same root over every header up to block
971,711. That root is the checkpoint in `res/coins.json`, with the block's hash, which Blockstream's
and mempool.space's own APIs name the same. A header below it is proven with one small call;
past it each header has to follow the last, with the work and the difficulty the rule gives, and
those come 2,016 a call. `.github/new-checkpoint.py` makes the next one. On the seed the tests use:
87 used addresses and 447 transactions read and proven in about 15 s, and each list, amount, block,
time and fee the same as Blockstream's REST API says.

One server, once, went slow for minutes and refused transactions while answering everything else.
A refusal of one kind therefore says nothing of the others: after a pause, the same kind refused
again is reason to ask the next server. Each server once: what none of them will give does not keep
the client going round them.

The history reads at 120 calls a second, and at 40 while a scanner reads rounds at 200 on the same
connection, which stays under the 250 at which the one server stopped. At the 500,000 transactions the
wallet is built for, with a raw transaction, a header and a proof for each, a first read takes hours
that way. It goes on in the background, new payments go ahead of it, and a start goes on where it stopped.

**What this means:** 200 reads a second is below the pace any of them complained about, so
reading the 335,000 or so addresses of 500 wallets that have no live place takes about 30
minutes. How many subscriptions a server takes was
not measured past 2,000 per connection: 40,000 stays below what the software allows by default
(ElectrumX 50,000 a session, Fulcrum 75,000 an IP).

## Ethereum: JSON-RPC endpoints

| Endpoint | Largest batch | `eth_getLogs` range | Topic list | Multicall3 sub-calls | Old state |
|---|---|---|---|---|---|
| ethereum-rpc.publicnode.com | 500 (1,000: HTTP 429) | 5,000 blocks; 10,000 asks for a token; a contract is required | Needs a contract | 2,000 (5,000: body too large) | Refused |
| eth-mainnet.public.blastapi.io | 500 (1,000: 965 answered) | **10 blocks** | | 5,000 | Refused by rate at the time |
| rpc.mevblocker.io | 1,000, at 6 ms a call | 50,000 blocks, any token; 10,000 results at most, with a hint | 1,000 (5,000 refused) | 2,000 (5,000: HTTP 413) | Yes |
| eth.drpc.org | **3** on the free plan | 10,000 | | | Yes |
| cloudflare-eth.com | **10** | | | | |
| gateway.tenderly.co/public/mainnet | Rate limited at 200 | | | 600 fine | |
| rpc.flashbots.net | Too many at 200 | | | `eth_call` not allowed | |
| 1rpc.io/eth | **Usage limit reached**: off the list | | | | |
| eth.llamarpc.com, eth.merkle.io, eth.api.onfinality.io, rpc.ankr.com/eth, eth-pokt.nodies.app | Refused, rate limited or need a key | | | | |

**The pace that holds** (one test a minute long each, after a minute's rest):

| Endpoint | Multicall3, 600 sub-calls | Nonces, plain calls |
|---|---|---|
| ethereum-rpc.publicnode.com | 2 a second: 118 of 118 | 20 a second: all fine. 50 a second: 429 after 39 s |
| eth-mainnet.public.blastapi.io | As fast as it answers, about 1.4 a second: 85 of 85 | 50 a second: all fine. 200 a second: 429 after 8 s |
| rpc.mevblocker.io | 1 a second: 45 of 46, one read timeout | 50 a second: all fine. About 125 a second, at its own pace: 117 of 120 rounds, three timeouts |

Together, 200 addresses a second with nonces (200 plain calls and one Multicall3) broke publicnode
after 6 seconds and blastapi after 8; mevblocker kept going at its own pace of about 1.6 s a round
(117 of 120).

**What this means:**

- **Balances cost almost nothing, nonces a lot.** Hundreds of balances go in one `eth_call`, so a
  pass over 50,000 addresses is under 100 calls, about a minute at one call and a bit a second.
  So nonces are read for new rows and otherwise rarely, as all they can still add is an address
  that received and sent everything between two passes.
- **No two endpoints allow the same thing.** Batch caps of 3 and 10 exist in the wild, so a chunk
  that fails halves the next, nonces learn the batch an endpoint takes, and each endpoint has
  its own pace.
- **Full history cannot come from these.** None has `trace_filter`, `ots_*` or `alchemy_*`, and
  `eth_getLogs` ranges run from 10 to 50,000 blocks against 26 million on the chain.
- **A whole block** is 280 to 540 KB, its receipts 0.5 to 1.2 MB; USDT alone has about 200 `Transfer`
  logs per block (130 KB). Following every block would cost well over 1 GB a day per chain.

## Native history without a key

Measured for stage 3, on `eth.blockscout.com`:

- `api/v2/addresses/{a}/transactions`: 50 transactions with full detail, 100 KB. Says
  `x-ratelimit-limit: 180` per about 30 seconds, and 30 calls a second apart all answered.
- `api/v2/addresses/{a}/token-transfers`: the same; 6 s the first time.
- The older `api?module=account&action=txlist` form: `x-ratelimit-limit: 10`.

Etherscan without a key: `Missing/Invalid API Key`. Ethplorer's `freekey` answers, limits not measured.
So native Ethereum history for used addresses can start without any key, at about 6 addresses a second.
