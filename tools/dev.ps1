#requires -version 5
<#
.SYNOPSIS
    AI Passport 一站构建脚本：编译 → 合并镜像 →（可选）烧录 →（可选）模拟器截图。

.EXAMPLE
    .\tools\dev.ps1                       # 只编译
    .\tools\dev.ps1 -Task all             # 编译 + 合并镜像
    .\tools\dev.ps1 -Task flash -Port COM5
    .\tools\dev.ps1 -Task shot            # 编译 + 合并 + 模拟器无头截图
    .\tools\dev.ps1 -Task all -Clean      # 全量重编
#>
param(
    [ValidateSet('build', 'flash', 'shot', 'all', 'monitor')]
    [string]$Task = 'build',

    [string]$Port,
    [switch]$Clean,
    [string]$Out = '',
    [string]$IdfPath = 'D:\aipassport\esp\esp-idf',
    [string]$EmulatorUrl = 'http://127.0.0.1:4190/',
    [int]$Baud = 460800
)

# 注意：idf.py / cmake 会把正常进度和 warning 写到 stderr，Windows PowerShell 5.1 会把它
# 当成 ErrorRecord，Stop 模式下直接误抛。所以这里用 Continue，成败一律看 $LASTEXITCODE。
$ErrorActionPreference = 'Continue'
if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$Bin = ''   # 编译后按 app 名推导为 <app>-full.bin：仓库自带 verify_firmware.py 认这个约定
if (-not $Out) { $Out = Join-Path $ProjectRoot 'build\emulator-shot.png' }

function Step($msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }
function Ok($msg)   { Write-Host "  [OK] $msg" -ForegroundColor Green }
function Fail($msg) { Write-Host "  [!!] $msg" -ForegroundColor Red }

function Enable-Idf {
    if (Get-Command idf.py -ErrorAction SilentlyContinue) { Ok 'ESP-IDF 环境已在当前会话中'; return }
    $export = Join-Path $IdfPath 'export.ps1'
    if (-not (Test-Path $export)) { throw "找不到 $export，检查 -IdfPath" }
    # export.ps1 会通过原生命令往 stderr 写进度，Stop 模式下会被误判成终止错误
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $export *> $null } finally { $ErrorActionPreference = $prev }
    if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) { throw 'export.ps1 执行后仍无 idf.py' }
    Ok 'ESP-IDF 环境已激活'
}

function Get-SerialPort {
    if ($Port) { return $Port }
    $found = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '\((COM\d+)\)' } |
        ForEach-Object { [regex]::Match($_.Name, '(COM\d+)').Value }
    if (-not $found) { throw '没有检测到串口设备。插上能传数据的 Type-C 线，或用 -Port COMx 指定。' }
    return @($found)[-1]
}

Push-Location $ProjectRoot
try {
    Step '1/4 激活工具链'
    Enable-Idf

    if ($Clean) {
        Step '清理 build/'
        idf.py fullclean
    }

    Step '2/4 编译'
    idf.py build
    if ($LASTEXITCODE -ne 0) { throw 'idf.py build 失败' }
    $app = Get-ChildItem build\*.bin | Where-Object { $_.Name -notmatch 'bootloader|partition-table|full' } | Select-Object -First 1
    Ok ("app = {0} ({1:N0} B)" -f $app.Name, $app.Length)

    if ($Task -in 'flash', 'shot', 'all', 'monitor') {
        Step '3/4 合并镜像'
        # 命名跟随 verify_firmware.py 的约定：<app 名>-full.bin
        $mergedName = "$($app.BaseName)-full.bin"
        $Bin = Join-Path $ProjectRoot "build\$mergedName"
        # 注意：merge-bin 在 build\ 内执行，-o 不能带 build\ 前缀
        idf.py merge-bin -o $mergedName
        if ($LASTEXITCODE -ne 0) { throw 'idf.py merge-bin 失败' }
        $full = Get-Item $Bin
        $head = [System.IO.File]::ReadAllBytes($full.FullName)[0]
        if ($head -ne 0xE9) { throw ("合并镜像首字节 0x{0:X2}，不是 0xE9，镜像可疑" -f $head) }
        Ok ("{0} = {1:N0} B，首字节 0xE9，可从 0x0 刷入" -f $full.Name, $full.Length)
    }

    switch ($Task) {
        'flash' {
            Step '4/4 烧录'
            $p = Get-SerialPort
            Ok "串口 $p，波特率 $Baud"
            idf.py -p $p -b $Baud flash
            if ($LASTEXITCODE -ne 0) { throw '烧录失败' }
            Ok '烧录完成'
        }
        'monitor' {
            Step '4/4 串口监视（Ctrl+] 退出）'
            $p = Get-SerialPort
            idf.py -p $p monitor
        }
        'shot' {
            Step '4/4 模拟器截图'
            if (-not (Get-Command node -ErrorAction SilentlyContinue)) { throw '需要 node（模拟器本身也依赖它）' }
            try { Invoke-WebRequest "$EmulatorUrl`healthz" -UseBasicParsing -TimeoutSec 5 | Out-Null } catch { throw "模拟器没在跑：$EmulatorUrl（先 cd D:\aipassport\simulator; npm start）" }
            & node (Join-Path $PSScriptRoot 'emulator-shot.mjs') $Bin $Out
            if ($LASTEXITCODE -ne 0) { throw '截图失败' }
            Ok "截图 $Out"
        }
        'all' {
            Ok '编译 + 镜像完成。烧录用 -Task flash，截图用 -Task shot'
        }
    }
    Write-Host "`n全部完成。" -ForegroundColor Green
}
catch {
    Fail $_.Exception.Message
    exit 1
}
finally {
    Pop-Location
}
