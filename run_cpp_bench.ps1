# -------------------------------
# C++ benchmarking script
# -------------------------------

$runs = 10
$maxWorkers = 12

# Executables to test
$exes = @(
    @{ Name = "O2"; Path = ".\cpp_indexer_O2.exe" },
    @{ Name = "O0"; Path = ".\indexer_O0.exe" }
    @{ Name = "O2_LTO"; Path = ".\indexer_O2_LTO.exe" }
)

$rows = @()

for ($w = 1; $w -le $maxWorkers; $w++) {

    $row = [ordered]@{
        Workers = $w
    }

    foreach ($e in $exes) {

        Write-Host "Running $($e.Name) with $w workers..."

        $times = @()

        for ($i = 0; $i -lt $runs; $i++) {
            $out = & $e.Path $w

            if ($out -notmatch "elapsed_sec=([0-9\.eE\+\-]+)") {
                Write-Error "Could not parse time from: $out"
                exit 1
            }

            $times += [double]$Matches[1]
        }

        $mean = ($times | Measure-Object -Average).Average
        $row[$e.Name] = [math]::Round($mean, 6)
    }

    $rows += [pscustomobject]$row
}

# Print table
$rows | Format-Table -AutoSize

# Save CSV for Excel / report
$rows | Export-Csv -NoTypeInformation -Path cpp_results.csv

Write-Host "`nSaved results to cpp_results.csv"
