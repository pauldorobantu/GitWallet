# Three scans of the public test seed against the live public servers:
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
Expect ($script:compared -gt 500) "compared $($script:compared) addresses"
Expect ($different.Count -eq 0) "read in rounds, every address reads the same as followed live, its status included"
$different | Select-Object -First 10 | ForEach-Object { "      $_" }
Expect ($rounds.rowCount -eq $live.rowCount) "and the windows are as long ($($rounds.rowCount) and $($live.rowCount) rows)"

$extended = Scan 'extended' @('--coins-file', (Resolve-Path "$PSScriptRoot/extra-network.json"))
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
