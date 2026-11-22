param(
    [string]$XNVSE = "$PSScriptRoot\..\external\NVSE",
    [string]$WebView2 = "$PSScriptRoot\..\external\Microsoft.Web.WebView2"
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..").Path
if (-not (Test-Path "$XNVSE\nvse\nvse\PluginAPI.h")) {
    throw "Clone xNVSE: git clone https://github.com/xNVSE/NVSE.git `"$XNVSE`""
}
if (-not (Test-Path "$WebView2\build\native\include\WebView2.h")) {
    $packageVersion = '1.0.4370.26'
    New-Item -ItemType Directory -Force -Path $WebView2 | Out-Null
    $zip = Join-Path $env:TEMP "microsoft.web.webview2.$packageVersion.zip"
    Invoke-WebRequest "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/$packageVersion" -OutFile $zip
    Expand-Archive -Path $zip -DestinationPath $WebView2 -Force
}
cmake -S $root -B "$root\build" -A Win32 "-DXNVSE_SOURCE_ROOT=$XNVSE" "-DWEBVIEW2_SDK_ROOT=$WebView2"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build "$root\build" --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "PipNet.dll is in $root\build\Release"
