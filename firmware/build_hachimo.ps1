# build_hachimo.ps1 —— hachimo_pet(HachimoDock Mosaico 移植)固件构建脚本。
# 规则同 build_weiqi.ps1:先数 build/.ninja_deps 里的 /D:/ 计数,非 0 才移除
# (GSP 生成物路径坑,known-issues #1);绝不动 .ninja_log。
param(
    [switch]$Clean   # fullclean(换 sdkconfig 配对后使用)
)

$ErrorActionPreference = "Stop"
$workspace = "D:\esp-mo\esp-mosaico-vibe-main"
$project = "projects/hachimo_pet"
$build = "$workspace\$project\build"
$idf = "D:\esp-mo\esp-idf"
$script = "$workspace\submodule\esp-mosaico-utils\mosaico-tools\skills\idf-low-noise-build\scripts\idf_low_noise_build.py"

if ($Clean -and (Test-Path $build)) {
    Write-Host "[build] clean: removing $build"
    Remove-Item $build -Recurse -Force
}

if (Test-Path "$build\.ninja_deps") {
    $count = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes("$build\.ninja_deps")) ; `
        $slashD = ([regex]::Matches($count, "/D:/")).Count
    if ($slashD -gt 0) {
        Write-Host "[build] removing stale .ninja_deps (/D:/ count=$slashD)"
        Remove-Item "$build\.ninja_deps" -Force
    } else {
        Write-Host "[build] keeping .ninja_deps (/D:/ count=0)"
    }
}

Write-Host "[build] building $project ..."
Push-Location $workspace
try {
    $env:IDF_PATH = $idf
    python $script --project $project build
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
} finally {
    Pop-Location
}
Write-Host "[build] OK"
