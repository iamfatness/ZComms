# Builds and stages a distributable ZComms zip.
#
# The output zip contains the Zoom Meeting SDK runtime, which is licensed for
# distribution INSIDE an app built against the Marketplace app identity, but
# not for posting to a public repository. So: this script writes dist\ locally
# and dist\ is gitignored -- hand the zip to operators directly; never commit
# or publish it as a public release asset.
#
# Signing: when the Microsoft developer account lands, the signtool call slots
# in at the marked point below; nothing else changes.
param(
    [string]$Version = "0.1.0"
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $repo "dist"
$stage = Join-Path $dist "ZComms-$Version"

Write-Host "== configure + build =="
cmake -S $repo -B "$repo\build" -A x64 | Out-Null
cmake --build "$repo\build" --config Release --target zcomms zcomms-tap zcomms-engine
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Host "== run test suites =="
# Through cmd so benign stderr chatter (speex prints warnings there) cannot
# trip PowerShell's error stream handling.
cmd /c "`"$repo\build\Release\zcomms_audio_tests.exe`" 2>nul" | Select-Object -Last 1
if ($LASTEXITCODE -ne 0) { throw "audio tests failed -- not releasing" }

Write-Host "== stage =="
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# The app and its tools.
Copy-Item "$repo\build\Release\zcomms.exe" $stage
Copy-Item "$repo\build\Release\zcomms-tap.exe" $stage
Copy-Item "$repo\build\Release\zcomms-engine.exe" $stage

# The WebView2 loader for the shell window (redistributable; the runtime
# itself is evergreen on the user's machine).
if (Test-Path "$repo\build\Release\WebView2Loader.dll") {
    Copy-Item "$repo\build\Release\WebView2Loader.dll" $stage
}

# The Zoom SDK runtime the exe loads at start (sdk.dll and its dependency
# tree). The whole bin directory: the SDK's loader pulls pieces lazily and a
# minimal set is a support incident waiting to happen.
Copy-Item "$repo\third_party\zoom-sdk\bin\*" $stage -Recurse

Copy-Item "$repo\tools\QUICKSTART.md" (Join-Path $stage "QUICKSTART.md")

# --- signing hook -----------------------------------------------------------
# signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 `
#   /f cert.pfx /p <pass> "$stage\zcomms.exe"
# ----------------------------------------------------------------------------

Write-Host "== zip =="
$zip = Join-Path $dist "ZComms-$Version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
$mb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "== done: $zip ($mb MB) =="
