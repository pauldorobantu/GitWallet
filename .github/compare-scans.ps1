# Scans of the public test seed against the live public servers:
#  - followed live, as a small wallet always is;
#  - read in rounds, as what does not fit a connection's live places is: every address,
#    its status and its balance must come out the same;
#  - with one more network in the list of coins, a file and no code: its rows must be
#    there and answered.
param([string] $Report = 'scans')
New-Item -ItemType Directory -Force $Report | Out-Null

function Scan([string] $Name, [string[]] $Extra) {
    $data = Join-Path $env:RUNNER_TEMP "gw-scan-$Name"
    $file = (Join-Path (Resolve-Path $Report) "$Name.json")
    $arguments = @('--data-dir', $data, '--import-test-mnemonic', '--scan-report', $file, '--timeout', '900') + $Extra
    & "$PSScriptRoot/start-gitwallet.ps1" -Arguments $arguments
    if ($LASTEXITCODE -ne 0) { throw "the $Name scan ended with exit code $LASTEXITCODE" }
    return Get-Content $file -Raw | ConvertFrom-Json
}

$failures = 0
function Expect([bool] $ok, [string] $what) {
    if ($ok) { "ok    $what" } else { "FAIL  $what"; $script:failures++ }
}

$live = Scan 'live' @()
$rounds = Scan 'rounds' @('--live-places', '10')

$bitcoinLive = $live.networks | Where-Object { $_.id -eq 'bitcoin' }
$bitcoinRounds = $rounds.networks | Where-Object { $_.id -eq 'bitcoin' }
Expect ($bitcoinLive.rounds -eq 0) "followed live: nothing read in rounds ($($bitcoinLive.live) live)"
Expect ($bitcoinRounds.rounds -gt 0 -and $bitcoinRounds.live -le 10) "with 10 live places: $($bitcoinRounds.live) live, $($bitcoinRounds.rounds) read in rounds"

function Differences($reference, $scan) {
    $byPath = @{}
    foreach ($row in $reference.rows) { $byPath["$($row.network) $($row.path)"] = $row }
    $script:compared = 0
    $found = @()
    foreach ($row in $scan.rows) {
        $other = $byPath["$($row.network) $($row.path)"]
        if (-not $other) { continue }
        $script:compared++
        if ($row.network -ne 'bitcoin') { continue }
        if ($row.address -ne $other.address -or $row.used -ne $other.used -or $row.status -ne $other.status `
            -or $row.confirmed -ne $other.confirmed -or $row.unconfirmed -ne $other.unconfirmed) {
            $found += "$($row.path) $($row.address): used $($other.used)/$($row.used), status $($other.status)/$($row.status), confirmed $($other.confirmed)/$($row.confirmed)"
        }
    }
    return ,$found
}

$different = Differences $live $rounds
# Anyone can pay this seed, and a transaction confirms when it confirms: minutes apart, a
# difference may be the chain's. Read once more, one that the code made stays.
if ($different.Count -gt 0) {
    "      $($different.Count) rows differ; reading both once more"
    $live = Scan 'live-again' @()
    $rounds = Scan 'rounds-again' @('--live-places', '10')
    $different = Differences $live $rounds
}
# The history those scans read: all of it, proven, adding up, and the same however it was read.
# A pending one's time is when this run first saw it, which says nothing.
function HistoryIds($scan) { return @($scan.history | ForEach-Object { if ($_.height -gt 0) { "$($_.txid) $($_.height) $($_.time) $($_.fee)" } else { "$($_.txid) pending $($_.fee)" } } | Sort-Object) }
$state = $live.historyState | Where-Object { $_.network -eq 'bitcoin' }
Expect ($state.transactions -gt 0) "the history has transactions ($($state.transactions))"
Expect ($state.addressesLeft -eq 0 -and $state.transactionsLeft -eq 0 -and $state.proofsLeft -eq 0) "and nothing left to read"
Expect ($state.refused -eq 0 -and $state.unproven -eq 0) "nothing refused, no proof that did not hold"
Expect ($state.differing -eq 0) "on every address the transactions add up to the balance"
$inBlocks = @($live.history | Where-Object { $_.height -gt 0 })
$proven = @($inBlocks | Where-Object { $_.proven })
Expect ($proven.Count -eq $inBlocks.Count) "every transaction in a block has its proof checked ($($proven.Count) of $($inBlocks.Count))"
$priced = @($live.history | Where-Object { $_.usdThen -gt 0 })
# No price at all is the source's bad hour, which says nothing of the wallet, as an explorer's below.
if ($priced.Count -eq 0) { "      no price came from the source in this run" }
Expect ($priced.Count -eq 0 -or $priced.Count -gt $live.history.Count / 2) "and most have what a coin was worth then ($($priced.Count) of $($live.history.Count))"
$historyDifferent = @(Compare-Object (HistoryIds $live) (HistoryIds $rounds))
# As with the rows: a block between two scans is the chain's.
if ($historyDifferent.Count -gt 0) {
    "      $($historyDifferent.Count) lines of the history differ; reading both once more"
    "      in rounds it stood at: $($rounds.historyState | ConvertTo-Json -Compress)"
    $historyDifferent | Select-Object -First 5 | ForEach-Object { "      $($_.SideIndicator) $($_.InputObject)" }
    $live = Scan 'live-history-again' @()
    $rounds = Scan 'rounds-history-again' @('--live-places', '10')
    $historyDifferent = @(Compare-Object (HistoryIds $live) (HistoryIds $rounds))
}
Expect ($historyDifferent.Count -eq 0) "read in rounds or live, the history is the same"
$historyDifferent | Select-Object -First 5 | ForEach-Object { "      $($_.InputObject)" }

# Another server first: other software, the same chain.
$coins = Get-Content "$PSScriptRoot/../res/coins.json" -Raw | ConvertFrom-Json
$bitcoinEntry = $coins.networks | Where-Object { $_.id -eq 'bitcoin' }
[array]::Reverse($bitcoinEntry.servers)
$reversed = Join-Path $env:RUNNER_TEMP 'gw-other-servers.json'
$coins | ConvertTo-Json -Depth 20 | Set-Content $reversed -Encoding utf8
$other = Scan 'other-server' @('--coins-file', $reversed)
$otherBitcoin = $other.networks | Where-Object { $_.id -eq 'bitcoin' }
$otherDifferent = @(Compare-Object (HistoryIds $live) (HistoryIds $other))
if ($otherDifferent.Count -gt 0) {
    "      $($otherDifferent.Count) lines differ from another server; reading once more"
    $live = Scan 'live-other-again' @()
    $other = Scan 'other-server-again' @('--coins-file', $reversed)
    $otherBitcoin = $other.networks | Where-Object { $_.id -eq 'bitcoin' }
    $otherDifferent = @(Compare-Object (HistoryIds $live) (HistoryIds $other))
}
Expect ($otherBitcoin.server -ne $bitcoinLive.server) "another server answers ($($otherBitcoin.server), not $($bitcoinLive.server))"
Expect ($otherDifferent.Count -eq 0) "and tells the same history"
$otherDifferent | Select-Object -First 5 | ForEach-Object { "      $($_.InputObject)" }

# A public explorer, which is no Electrum server at all. Being throttled says nothing: a difference does.
python -I "$PSScriptRoot/compare-history.py" (Join-Path (Resolve-Path $Report) 'live.json')
if ($LASTEXITCODE -ne 0) { "FAIL  the explorer says otherwise"; $script:failures++ } else { "ok    an explorer's history is the same, or it could not be asked" }

Expect ($script:compared -gt 500) "compared $($script:compared) addresses"
Expect ($different.Count -eq 0) "read in rounds, every address reads the same as followed live, its status included"
$different | Select-Object -First 10 | ForEach-Object { "      $_" }
Expect ($rounds.rowCount -eq $live.rowCount) "and the windows are as long ($($rounds.rowCount) and $($live.rowCount) rows)"

# The list as it is, with one more network: a file, no code.
$withBase = Get-Content "$PSScriptRoot/../res/coins.json" -Raw | ConvertFrom-Json
$withBase.networks += (Get-Content "$PSScriptRoot/extra-network.json" -Raw | ConvertFrom-Json).networks
$extendedFile = Join-Path $env:RUNNER_TEMP 'gw-extended.json'
$withBase | ConvertTo-Json -Depth 20 | Set-Content $extendedFile -Encoding utf8
$extended = Scan 'extended' @('--coins-file', $extendedFile)
$base = $extended.networks | Where-Object { $_.id -eq 'base' }
Expect ($null -ne $base) "the list of coins from a file adds a network"
Expect ($base.rows -gt 0 -and $base.unchecked -eq 0) "it has its rows, all answered ($($base.rows)), on $($base.server)"
Expect ($null -ne $extended.totals.'base/usdc' -and $null -ne $extended.pricesUsd.'base/usdc') "its token has a total and a price entry"
Expect ($extended.pricesUsd.'base/eth' -gt 0) "its coin has a price ($($extended.pricesUsd.'base/eth'))"
$first = $extended.rows | Where-Object { $_.network -eq 'base' -and $_.path -eq "m/44'/60'/0'/0/0" }
$firstEthereum = $live.rows | Where-Object { $_.network -eq 'ethereum' -and $_.path -eq "m/44'/60'/0'/0/0" }
Expect ($first -and $first.address -eq $firstEthereum.address) "and the same seed has the same address there"

"failures: $failures"
exit $failures
