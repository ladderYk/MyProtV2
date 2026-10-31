# scripts/webui-smoke.ps1 -- headless WebUI regression via Edge DevTools Protocol.
#
# Why: ci.ps1 covers the C++ side (unit + E2E) but nothing guards the Vue UI.
#      This script rebuilds dist/, boots MyProt.App on a free loopback port,
#      drives headless Edge over CDP and asserts the main user flows.
#
# What it checks:
#   shell layout + nav, overview cards/checklist, help drawer, protocol editor
#   (search / JSON mode / self-check / unsaved-changes guard), tags tree,
#   live table (search / inline write / write drawer / SSE toggle), simulator, breakpoints,
#   and a clean console/network (no JS errors, 429 or failed requests).
#
# Requirements: Windows, Microsoft Edge, Node.js (unless -SkipUiBuild),
#               a built MyProt.App (default build\Release\x64\bin\MyProt.App.exe).
#
# Exit codes: 0 = all checks passed, 1 = check failures, 2 = setup failure.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\webui-smoke.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\webui-smoke.ps1 -SkipUiBuild
#   powershell -ExecutionPolicy Bypass -File scripts\webui-smoke.ps1 -KeepAlive
#
# NOTE: file is intentionally ASCII-only (same reason as ci.ps1).

param(
    [string]$AppExe = "",
    [string]$ConfigDir = "configs",
    [int]$ApiPort = 0,
    [int]$CdpPort = 0,
    [string]$BrowserPath = "",
    [switch]$SkipUiBuild,
    [switch]$KeepAlive,
    [int]$StartupTimeoutSec = 45
)

$ErrorActionPreference = "Continue"

$root = Split-Path -Parent $PSScriptRoot
$webuiDir = Join-Path $root "webui"
$distIndex = Join-Path $webuiDir "dist\index.html"

$script:checks = New-Object System.Collections.ArrayList
$script:consoleErrors = New-Object System.Collections.ArrayList
$script:consoleWarnings = New-Object System.Collections.ArrayList
$script:pageExceptions = New-Object System.Collections.ArrayList
$script:failedRequests = New-Object System.Collections.ArrayList
$script:throttled = New-Object System.Collections.ArrayList
$script:serverErrors = New-Object System.Collections.ArrayList

$script:cdpId = 0
$script:ws = $null
$script:appProc = $null
$script:browserProc = $null
$script:edgeProfile = $null
$script:appOut = $null
$script:appErr = $null
$script:browserOut = $null
$script:browserErr = $null
$baseUrl = ""

# ---------------------------------------------------------------- utilities

function Write-Head([string]$text) {
    Write-Host ""
    Write-Host "============================================================"
    Write-Host " $text"
    Write-Host "============================================================"
}

function Get-FreePort {
    $listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
    $listener.Stop()
    return $port
}

function Check {
    param([string]$Name, [bool]$Ok, [string]$Detail = "")
    [void]$script:checks.Add([pscustomobject]@{ Name = $Name; Ok = $Ok })
    if ($Ok) {
        Write-Host ("  [PASS] {0}" -f $Name) -ForegroundColor Green
    } elseif ($Detail) {
        Write-Host ("  [FAIL] {0} -- {1}" -f $Name, $Detail) -ForegroundColor Red
    } else {
        Write-Host ("  [FAIL] {0}" -f $Name) -ForegroundColor Red
    }
}

function Check-Js {
    param([string]$Name, [string]$Expr, [string]$Detail = "")
    try {
        $value = Invoke-Js $Expr
        if ([bool]$value) {
            Check $Name $true
        } elseif ($Detail) {
            Check $Name $false $Detail
        } else {
            Check $Name $false ("value=" + [string]$value)
        }
    } catch {
        Check $Name $false $_.Exception.Message
    }
}

function Check-Wait {
    param([string]$Name, [string]$Expr, [int]$TimeoutSec = 12)
    $ok = Wait-Js $Expr $TimeoutSec
    Check $Name $ok ("timeout after " + $TimeoutSec + "s: " + $Expr)
}

# ---------------------------------------------------------------- CDP client

function Receive-CdpMessage {
    $buffer = New-Object byte[] 65536
    $stream = New-Object System.IO.MemoryStream
    try {
        do {
            $segment = New-Object System.ArraySegment[byte] -ArgumentList @(, $buffer)
            $cts = New-Object System.Threading.CancellationTokenSource
            $cts.CancelAfter(30000)
            $result = $script:ws.ReceiveAsync($segment, $cts.Token).GetAwaiter().GetResult()
            if ($result.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) {
                throw "CDP websocket closed"
            }
            $stream.Write($buffer, 0, $result.Count)
        } while (-not $result.EndOfMessage)
        return [System.Text.Encoding]::UTF8.GetString($stream.ToArray())
    } finally {
        $stream.Dispose()
    }
}

function Send-CdpRaw($payload) {
    $json = $payload | ConvertTo-Json -Compress -Depth 20
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $segment = New-Object System.ArraySegment[byte] -ArgumentList @(, $bytes)
    $script:ws.SendAsync($segment, [System.Net.WebSockets.WebSocketMessageType]::Text, $true,
                         [System.Threading.CancellationToken]::None).GetAwaiter().GetResult() | Out-Null
}

function Handle-CdpEvent($message) {
    $method = $message.method
    if ($method -eq "Runtime.consoleAPICalled") {
        $parts = @()
        foreach ($arg in $message.params.args) {
            if ($null -ne $arg.value) { $parts += [string]$arg.value }
            elseif ($arg.description) { $parts += [string]$arg.description }
        }
        $text = ($parts -join " ")
        if ($message.params.type -eq "error") { [void]$script:consoleErrors.Add($text) }
        elseif ($message.params.type -eq "warning") { [void]$script:consoleWarnings.Add($text) }
    } elseif ($method -eq "Runtime.exceptionThrown") {
        $details = $message.params.exceptionDetails
        $text = $details.text
        if ($details.exception -and $details.exception.description) {
            $text = [string]$details.exception.description
        }
        [void]$script:pageExceptions.Add($text)
    } elseif ($method -eq "Log.entryAdded") {
        $entry = $message.params.entry
        $text = ("[" + $entry.source + "] " + $entry.text)
        if ($entry.level -eq "error") { [void]$script:consoleErrors.Add($text) }
        elseif ($entry.level -eq "warning") { [void]$script:consoleWarnings.Add($text) }
    } elseif ($method -eq "Network.responseReceived") {
        $response = $message.params.response
        if ($response.status -eq 429) {
            [void]$script:throttled.Add([string]$response.url)
        } elseif ($response.status -ge 500) {
            [void]$script:serverErrors.Add(("{0} {1}" -f $response.status, $response.url))
        }
    } elseif ($method -eq "Network.loadingFailed") {
        $errorText = [string]$message.params.errorText
        if ($errorText -notmatch "ERR_ABORTED") {
            [void]$script:failedRequests.Add($errorText)
        }
    } elseif ($method -eq "Page.javascriptDialogOpening") {
        # Nothing in the smoke flow expects a native dialog; accept it so a
        # stray confirm() can never wedge the audit.
        $script:cdpId++
        Send-CdpRaw @{
            id = $script:cdpId
            method = "Page.handleJavaScriptDialog"
            params = @{ accept = $true }
        }
    }
}

function Send-Cdp {
    param([string]$Method, $Params = $null)
    $script:cdpId++
    $id = $script:cdpId
    $payload = @{ id = $id; method = $Method }
    if ($null -ne $Params) { $payload.params = $Params }
    Send-CdpRaw $payload

    while ($true) {
        $raw = Receive-CdpMessage
        $message = $raw | ConvertFrom-Json
        if ($null -ne $message.id -and $message.id -eq $id) {
            if ($message.error) {
                throw ("CDP {0} failed: {1}" -f $Method, ($message.error | ConvertTo-Json -Compress))
            }
            return $message.result
        }
        if ($message.method) { Handle-CdpEvent $message }
    }
}

function Invoke-Js {
    param([string]$Expression)
    $result = Send-Cdp "Runtime.evaluate" @{
        expression = $Expression
        awaitPromise = $true
        returnByValue = $true
        userGesture = $true
    }
    if ($result.exceptionDetails) {
        $text = $result.exceptionDetails.text
        if ($result.exceptionDetails.exception -and $result.exceptionDetails.exception.description) {
            $text = [string]$result.exceptionDetails.exception.description
        }
        throw ("JS exception: " + $text)
    }
    return $result.result.value
}

function Wait-Js {
    param([string]$Expression, [int]$TimeoutSec = 12)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        try {
            if (Invoke-Js $Expression) { return $true }
        } catch {
            # element not there yet / context replaced during navigation
        }
        Start-Sleep -Milliseconds 200
    }
    return $false
}

function Set-Viewport {
    param([int]$Width, [int]$Height)
    Send-Cdp "Emulation.setDeviceMetricsOverride" @{
        width = $Width
        height = $Height
        deviceScaleFactor = 1
        mobile = $false
    } | Out-Null
    Start-Sleep -Milliseconds 250
}

# ---------------------------------------------------------------- setup

function Resolve-AppExe {
    if ($AppExe) {
        if (Test-Path -LiteralPath $AppExe) { return (Resolve-Path -LiteralPath $AppExe).Path }
        throw ("AppExe not found: " + $AppExe)
    }
    $candidates = @(
        (Join-Path $root "build\Release\x64\bin\MyProt.App.exe"),
        (Join-Path $root "bin\Release\MyProt.App.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw "MyProt.App.exe not found; build first (scripts\build.bat Release x64) or pass -AppExe"
}

function Resolve-Browser {
    if ($BrowserPath) {
        if (Test-Path -LiteralPath $BrowserPath) { return (Resolve-Path -LiteralPath $BrowserPath).Path }
        throw ("Browser not found: " + $BrowserPath)
    }
    $candidates = @()
    if (${env:ProgramFiles(x86)}) {
        $candidates += (Join-Path ${env:ProgramFiles(x86)} "Microsoft\Edge\Application\msedge.exe")
    }
    if ($env:ProgramFiles) {
        $candidates += (Join-Path $env:ProgramFiles "Microsoft\Edge\Application\msedge.exe")
    }
    if ($env:LOCALAPPDATA) {
        $candidates += (Join-Path $env:LOCALAPPDATA "Microsoft\Edge\Application\msedge.exe")
    }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    $command = Get-Command msedge.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    throw "Microsoft Edge not found; pass -BrowserPath"
}

function Remove-WithRetry([string]$Path) {
    for ($i = 0; $i -lt 5; $i++) {
        try {
            if (Test-Path -LiteralPath $Path) {
                Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
            }
            return
        } catch {
            Start-Sleep -Milliseconds 300
        }
    }
}

function Stop-SmokeProcesses {
    if ($script:browserProc -and -not $script:browserProc.HasExited) {
        try { Stop-Process -Id $script:browserProc.Id -Force -ErrorAction SilentlyContinue } catch { }
    }
    # Edge renderers survive the browser process kill sometimes; match the profile.
    if ($script:edgeProfile) {
        try {
            Get-CimInstance Win32_Process -Filter "Name = 'msedge.exe'" |
                Where-Object { $_.CommandLine -and $_.CommandLine.Contains($script:edgeProfile) } |
                ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
        } catch { }
    }
    if ($script:appProc -and -not $script:appProc.HasExited) {
        try { Stop-Process -Id $script:appProc.Id -Force -ErrorAction SilentlyContinue } catch { }
    }
    Remove-WithRetry $script:edgeProfile
}

function Show-AppLogTail {
    if ($script:appErr -and (Test-Path -LiteralPath $script:appErr)) {
        Write-Host "--- MyProt.App stderr (tail) ---" -ForegroundColor Red
        Get-Content -LiteralPath $script:appErr -Tail 20 -ErrorAction SilentlyContinue |
            ForEach-Object { Write-Host ("   " + $_) -ForegroundColor Red }
    }
    if ($script:appOut -and (Test-Path -LiteralPath $script:appOut)) {
        Write-Host "--- MyProt.App stdout (tail) ---" -ForegroundColor Red
        Get-Content -LiteralPath $script:appOut -Tail 20 -ErrorAction SilentlyContinue |
            ForEach-Object { Write-Host ("   " + $_) -ForegroundColor Red }
    }
}

# ---------------------------------------------------------------- main

try {
    # 1. UI build -----------------------------------------------------------
    if (-not $SkipUiBuild) {
        $npm = Get-Command npm -ErrorAction SilentlyContinue
        if ($npm) {
            Write-Head "WEBUI BUILD (vite)"
            Push-Location $webuiDir
            & $npm.Source run build
            $buildCode = $LASTEXITCODE
            Pop-Location
            if ($buildCode -ne 0) {
                Write-Host ("webui build failed (exit={0})" -f $buildCode) -ForegroundColor Red
                exit 2
            }
        } elseif (Test-Path -LiteralPath $distIndex) {
            Write-Host "[WARN] npm not found; using existing webui/dist" -ForegroundColor Yellow
        } else {
            Write-Host "npm not found and webui/dist is missing" -ForegroundColor Red
            exit 2
        }
    } elseif (-not (Test-Path -LiteralPath $distIndex)) {
        Write-Host "webui/dist is missing (and -SkipUiBuild was set)" -ForegroundColor Red
        exit 2
    }

    # 2. start the app ------------------------------------------------------
    $exe = Resolve-AppExe
    $browser = Resolve-Browser
    if ($ApiPort -eq 0) { $ApiPort = Get-FreePort }
    if ($CdpPort -eq 0) {
        do { $CdpPort = Get-FreePort } while ($CdpPort -eq $ApiPort)
    }
    if ($CdpPort -eq $ApiPort) { throw "ApiPort and CdpPort must differ" }
    $configPath = if ([System.IO.Path]::IsPathRooted($ConfigDir)) { $ConfigDir } else { Join-Path $root $ConfigDir }
    if (-not (Test-Path -LiteralPath (Join-Path $configPath "tags.json"))) {
        throw ("config directory has no tags.json: " + $configPath)
    }

    $stamp = [guid]::NewGuid().ToString("N")
    $script:appOut = Join-Path $env:TEMP ("myprot-ui-smoke-app-" + $stamp + ".out.log")
    $script:appErr = Join-Path $env:TEMP ("myprot-ui-smoke-app-" + $stamp + ".err.log")
    $script:browserOut = Join-Path $env:TEMP ("myprot-ui-smoke-edge-" + $stamp + ".out.log")
    $script:browserErr = Join-Path $env:TEMP ("myprot-ui-smoke-edge-" + $stamp + ".err.log")

    Write-Head ("START APP  " + $exe)
    $appArgs = ('--config "{0}" --port {1} --bind 127.0.0.1' -f $configPath, $ApiPort)
    $script:appProc = Start-Process -FilePath $exe -ArgumentList $appArgs -WorkingDirectory $root `
        -RedirectStandardOutput $script:appOut -RedirectStandardError $script:appErr `
        -WindowStyle Hidden -PassThru

    $baseUrl = ("http://127.0.0.1:{0}" -f $ApiPort)
    $deadline = (Get-Date).AddSeconds($StartupTimeoutSec)
    $ready = $false
    while ((Get-Date) -lt $deadline) {
        if ($script:appProc.HasExited) { break }
        try {
            $probe = Invoke-WebRequest -UseBasicParsing -Uri ($baseUrl + "/api/config/protocols") -TimeoutSec 2
            if ($probe.StatusCode -eq 200) { $ready = $true; break }
        } catch { }
        Start-Sleep -Milliseconds 300
    }
    if (-not $ready) {
        Write-Host "MyProt.App did not become ready" -ForegroundColor Red
        Show-AppLogTail
        exit 2
    }
    Write-Host ("  app ready: {0}" -f $baseUrl)

    # 3. start headless Edge ------------------------------------------------
    $script:edgeProfile = Join-Path $env:TEMP ("myprot-ui-smoke-edge-" + $stamp)
    New-Item -ItemType Directory -Path $script:edgeProfile -Force | Out-Null

    Write-Head "START EDGE (headless)"
    $edgeArgs = @(
        "--headless=new",
        "--remote-debugging-port=$CdpPort",
        "--user-data-dir=$script:edgeProfile",
        "--remote-allow-origins=*",
        "--no-first-run",
        "--no-default-browser-check",
        "--disable-gpu",
        "--disable-extensions",
        "--disable-background-networking",
        "--disable-sync",
        "--disable-features=Translate",
        "--window-size=1440,900",
        "about:blank"
    )
    $script:browserProc = Start-Process -FilePath $browser -ArgumentList $edgeArgs `
        -RedirectStandardOutput $script:browserOut -RedirectStandardError $script:browserErr `
        -WindowStyle Hidden -PassThru

    $target = $null
    $deadline = (Get-Date).AddSeconds(25)
    while ((Get-Date) -lt $deadline) {
        try {
            $targets = Invoke-RestMethod -Uri ("http://127.0.0.1:{0}/json/list" -f $CdpPort) -TimeoutSec 2
            $target = @($targets | Where-Object { $_.type -eq "page" }) | Select-Object -First 1
            if ($target) { break }
        } catch { }
        Start-Sleep -Milliseconds 300
    }
    if (-not $target) { throw "Edge DevTools endpoint did not expose a page target" }

    $script:ws = New-Object System.Net.WebSockets.ClientWebSocket
    $script:ws.ConnectAsync([Uri]$target.webSocketDebuggerUrl,
                            [System.Threading.CancellationToken]::None).GetAwaiter().GetResult() | Out-Null
    Write-Host ("  cdp attached: " + $target.id)

    Send-Cdp "Page.enable" | Out-Null
    Send-Cdp "Runtime.enable" | Out-Null
    Send-Cdp "Log.enable" | Out-Null
    Send-Cdp "Network.enable" | Out-Null
    Set-Viewport 1440 900

    Write-Head "NAVIGATE"
    Send-Cdp "Page.navigate" @{ url = $baseUrl + "/" } | Out-Null
    if (-not (Wait-Js 'document.readyState === "complete" && !!document.querySelector(".topbar")' 40)) {
        Show-AppLogTail
        throw "WebUI shell did not render (topbar missing)"
    }

    # Helper injected once into the page; every assertion below uses it.
    $helper = @'
window.__smoke = {
  q: function (s) { return document.querySelector(s); },
  qa: function (s) { return Array.from(document.querySelectorAll(s)); },
  txt: function (s) { var e = document.querySelector(s); return e ? (e.innerText || "").trim() : ""; },
  click: function (s) { var e = document.querySelector(s); if (!e) return false; e.click(); return true; },
  clickText: function (sel, t) {
    var e = this.qa(sel).find(function (x) { return (x.innerText || "").trim().indexOf(t) >= 0; });
    if (!e) return false; e.click(); return true;
  },
  count: function (s) { return document.querySelectorAll(s).length; },
  visible: function (s) {
    var e = document.querySelector(s); if (!e) return false;
    var r = e.getBoundingClientRect(); return r.width > 0 && r.height > 0;
  },
  rect: function (s) {
    var e = document.querySelector(s); if (!e) return null;
    var r = e.getBoundingClientRect();
    return { x: Math.round(r.x), y: Math.round(r.y), w: Math.round(r.width), h: Math.round(r.height) };
  },
  setInput: function (s, v) {
    var e = document.querySelector(s); if (!e) return false;
    var desc = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(e), "value");
    if (desc && desc.set) desc.set.call(e, v); else e.value = v;
    e.dispatchEvent(new Event("input", { bubbles: true }));
    e.dispatchEvent(new Event("change", { bubbles: true }));
    return true;
  },
  nav: function (i) { var n = document.querySelectorAll(".nav-item")[i]; if (!n) return false; n.click(); return true; },
  sleep: function (ms) { return new Promise(function (r) { setTimeout(r, ms); }); }
};
true
'@
    Invoke-Js $helper | Out-Null

    # =======================================================================
    Write-Head "SHELL"
    Check-Js "topbar rendered (>= 40px)" 'document.querySelector(".topbar").getBoundingClientRect().height >= 40'
    Check-Js "topbar height is 56px" 'Math.round(document.querySelector(".topbar").getBoundingClientRect().height) === 56'
    Check-Js "nav has 6 items" 'document.querySelectorAll(".nav-item").length === 6'
    Check-Js "overview is the default view" 'document.querySelectorAll(".nav-item")[0].classList.contains("active") && !!document.querySelector(".overview")'
    Check-Js "brand rendered" 'document.querySelector(".brand-name") && document.querySelector(".brand-name").innerText.trim() === "MyProt"'
    Check-Js "status chip shows device/tag counters" '/\d+\s*\/\s*\d+/.test(document.querySelector(".tb-status") ? document.querySelector(".tb-status").innerText : "")'
    Check-Js "content fills the viewport" 'document.querySelector(".content").getBoundingClientRect().height > 300'
    # requireAuth=false (default config): no login gate and no logout action
    Check-Js "no login gate when auth is disabled" '!document.querySelector(".gate")'
    Check-Js "logout hidden when auth is disabled" '![].slice.call(document.querySelectorAll(".tb-actions .tb-btn")).some(function (b) { return b.textContent.replace(/\s/g, "") === "\u9000\u51fa"; })'

    # =======================================================================
    Write-Head "OVERVIEW"
    Check-Js "4 stat cards" 'document.querySelectorAll(".stat-card").length === 4'
    # 开始使用区有两种形态: 未就绪时展开 4 步清单, 就绪后折叠为一行摘要
    Check-Js "checklist or ready summary is shown" '(document.querySelectorAll(".check-item").length === 4) || !!document.querySelector(".ready-row")'
    Check-Js "progress bar or ready summary is shown" '!!document.querySelector(".progress-bar") || !!document.querySelector(".ready-row")'
    Check-Js "quick actions removed" '!document.querySelector(".quick-grid")'
    Check-Js "welcome banner removed" '!document.querySelector(".welcome")'
    Check-Js "health status is inline" 'document.querySelectorAll(".health-inline").length >= 1'
    Check-Wait "device overview table has rows" 'document.querySelectorAll(".dev-table tbody tr").length >= 1' 15
    $overviewDevices = Invoke-Js 'document.querySelectorAll(".dev-table tbody tr").length'

    # =======================================================================
    Write-Head "HELP DRAWER"
    Invoke-Js 'document.querySelectorAll(".tb-actions .tb-btn")[1].click(); true' | Out-Null
    Check-Wait "help drawer opens" '!!document.querySelector(".help-drawer") && window.__smoke.visible(".help-drawer")' 8
    Check-Js "help drawer is 440px wide" '(function () { var r = window.__smoke.rect(".help-drawer"); return r && Math.abs(r.w - 440) <= 2; })()'
    Check-Js "help drawer has quick-start steps" 'document.querySelectorAll(".help-drawer .step").length >= 3'
    Invoke-Js 'document.querySelector(".help-drawer .foot button").click(); true' | Out-Null
    Check-Wait "help drawer closes" '!window.__smoke.visible(".help-drawer")' 8

    # =======================================================================
    Write-Head "PROTOCOLS"
    Invoke-Js 'window.__smoke.nav(4); true' | Out-Null
    Check-Wait "protocol workbench opens" '!!document.querySelector(".cfg-list") && document.querySelectorAll(".cfg-items li").length > 0 && !!document.querySelector(".toolbar")' 25
    Check-Js "protocol list is populated" 'document.querySelectorAll(".cfg-items li").length >= 5'
    Check-Wait "first protocol is selected" '!!document.querySelector(".cfg-items li.active")' 15
    Check-Wait "editor shows a json file" '/\.json$/.test(window.__smoke.txt(".file-name"))' 15
    $protoTotal = Invoke-Js 'document.querySelectorAll(".cfg-items li").length'

    Invoke-Js 'document.querySelectorAll(".segmented button")[1].click(); true' | Out-Null
    Check-Wait "JSON mode renders textarea" '!!document.querySelector("textarea.area")' 8
    Check-Js "JSON parses (status valid)" '!!document.querySelector(".editor .status.valid")'
    Check-Js "JSON contains schemaVersion" 'document.querySelector("textarea.area").value.indexOf("\"schemaVersion\"") >= 0'
    Invoke-Js 'document.querySelectorAll(".segmented button")[0].click(); true' | Out-Null
    Check-Wait "form mode returns" '!!document.querySelector(".pform")' 8
    Check-Js "protocol form has section tabs" 'document.querySelectorAll(".pform .el-tabs__item").length >= 3'

    Invoke-Js 'window.__smoke.setInput(".cfg-search input", "modbus")' | Out-Null
    Start-Sleep -Milliseconds 300
    Check-Js "protocol search filters the list" ('(function () { var n = document.querySelectorAll(".cfg-items li").length; return n >= 1 && n < ' + $protoTotal + '; })()')
    Invoke-Js 'window.__smoke.setInput(".cfg-search input", "")' | Out-Null
    Start-Sleep -Milliseconds 300
    Check-Js "clearing the search restores the list" ('document.querySelectorAll(".cfg-items li").length === ' + $protoTotal)

    Invoke-Js 'window.__smoke.clickText(".cfg-items li", "modbus-tcp")' | Out-Null
    Check-Wait "switch to modbus-tcp" 'window.__smoke.txt(".file-name").indexOf("modbus-tcp") === 0' 15
    # Backups are user-generated and not tracked in git: assert either branch.
    $backupResp = Invoke-RestMethod -Uri ($baseUrl + "/api/config/protocols/modbus-tcp/backups") -TimeoutSec 5
    $backupCount = 0
    if ($null -ne $backupResp) { $backupCount = @($backupResp).Count }
    if ($backupCount -gt 0) {
        Check-Wait "backup dropdown is available" 'window.__smoke.qa(".toolbar button").some(function (b) { return b.innerText.indexOf("\u5386\u53f2\u5907\u4efd") >= 0; })' 10
    } else {
        Check-Js "backup dropdown hidden without backups" '!window.__smoke.qa(".toolbar button").some(function (b) { return b.innerText.indexOf("\u5386\u53f2\u5907\u4efd") >= 0; })'
    }

    Invoke-Js 'window.__smoke.clickText(".toolbar button", "\u81ea\u68c0")' | Out-Null
    Check-Wait "self-check reports a result" '!!document.querySelector(".msg.ok") || !!document.querySelector(".issue-banner")' 20

    # Unsaved-changes guard: dirty the JSON, then try to leave the page.
    Invoke-Js 'document.querySelectorAll(".segmented button")[1].click(); true' | Out-Null
    Check-Wait "JSON mode for dirty guard" '!!document.querySelector("textarea.area")' 8
    Invoke-Js '(function () { var t = document.querySelector("textarea.area"); t.value = t.value.replace("{", "{\"__smoke_dirty\":true,"); t.dispatchEvent(new Event("input", { bubbles: true })); return true; })()' | Out-Null
    Check-Wait "editing marks the config dirty" '!!document.querySelector(".dirty-mark")' 8
    Invoke-Js 'window.__confirmNative = window.confirm; window.confirm = function () { return false; }; true' | Out-Null
    Invoke-Js 'window.__smoke.nav(0); true' | Out-Null
    Start-Sleep -Milliseconds 500
    Check-Js "guard keeps you on the page when cancelled" '!!document.querySelector(".workbench") && document.querySelectorAll(".nav-item")[4].classList.contains("active")'
    Invoke-Js 'window.confirm = window.__confirmNative; window.confirm = function () { return true; }; true' | Out-Null
    Invoke-Js 'window.__smoke.nav(0); true' | Out-Null
    Check-Wait "guard lets you leave when confirmed" '!!document.querySelector(".overview")' 12

    # =======================================================================
    Write-Head "TAGS"
    Invoke-Js 'window.__smoke.nav(5); true' | Out-Null
    Check-Wait "tags workbench opens" '!!document.querySelector(".tform")' 20
    Check-Wait "device tree is populated" 'document.querySelectorAll(".dt-dev").length >= 1' 15
    Check-Js "device tree matches the overview table" ('document.querySelectorAll(".dt-dev").length === ' + $overviewDevices)
    Check-Wait "tag nodes are listed" 'document.querySelectorAll(".dt-tag-name").length >= 10' 15
    Invoke-Js 'document.querySelector(".dt-dev-head").click(); true' | Out-Null
    Start-Sleep -Milliseconds 300
    Check-Js "selecting a device shows its context" '!!document.querySelector(".ctx-bar .ctx-cur")'
    Check-Js "device view shows info and connection together" '!!document.querySelector(".dev-pane") && document.querySelectorAll(".dev-pane .blk").length === 2'
    Check-Js "device view has no tabs" '!document.querySelector(".dev-pane .el-tabs")'
    Check-Js "device connection fields visible" 'document.querySelectorAll(".dev-pane .el-form-item").length >= 6'
    Invoke-Js 'document.querySelector(".dt-tags li").click(); true' | Out-Null
    Start-Sleep -Milliseconds 300
    Check-Js "selecting a tag shows its breadcrumb" '!!document.querySelector(".ctx-bar .ctx-dev") && !!document.querySelector(".ctx-bar .ctx-cur")'

    # =======================================================================
    Write-Head "LIVE"
    Invoke-Js 'window.__smoke.nav(1); true' | Out-Null
    Check-Wait "live panel opens" '!!document.querySelector(".live") && document.querySelectorAll(".live-table tbody tr:not(.group-row)").length >= 5' 25
    $liveTotal = Invoke-Js 'document.querySelectorAll(".live-table tbody tr:not(.group-row)").length'
    Invoke-Js 'window.__smoke.setInput(".filter-bar .search", "coil")' | Out-Null
    Start-Sleep -Milliseconds 400
    Check-Js "live search filters rows" ('(function () { var n = document.querySelectorAll(".live-table tbody tr:not(.group-row)").length; return n >= 1 && n < ' + $liveTotal + '; })()')
    Invoke-Js 'window.__smoke.setInput(".filter-bar .search", "")' | Out-Null
    Start-Sleep -Milliseconds 400
    Check-Js "clearing live search restores rows" ('document.querySelectorAll(".live-table tbody tr:not(.group-row)").length >= ' + $liveTotal)

    # inline write: double-click the value cell of the first writable row, then cancel with Esc
    Invoke-Js '(function () { var rows = document.querySelectorAll(".live-table tbody tr:not(.group-row)"); for (var i = 0; i < rows.length; ++i) { if (rows[i].querySelector(".btn.tiny")) { var td = rows[i].querySelector("td.value"); td.dispatchEvent(new MouseEvent("dblclick", { bubbles: true })); return true; } } return false; })()' | Out-Null
    Check-Wait "inline write input opens" '!!document.querySelector(".live-table input.inline-input")' 5
    Invoke-Js '(function () { var el = document.querySelector(".live-table input.inline-input"); if (!el) return false; el.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true })); return true; })()' | Out-Null
    Check-Wait "inline write cancels on Esc" '!document.querySelector(".live-table input.inline-input")' 5

    Invoke-Js 'document.querySelector(".live-table .btn.tiny").click(); true' | Out-Null
    Check-Wait "write drawer opens" '!!document.querySelector(".el-drawer") && window.__smoke.visible(".el-drawer")' 8
    Check-Js "write drawer is 420px wide" '(function () { var r = window.__smoke.rect(".el-drawer"); return r && Math.abs(r.w - 420) <= 2; })()'
    Check-Js "write drawer has value + bytes inputs" 'document.querySelectorAll(".write-body input.f-input").length >= 2'
    Invoke-Js 'window.__smoke.clickText(".write-foot button", "\u53d6\u6d88")' | Out-Null
    Check-Wait "write drawer closes" '!window.__smoke.visible(".el-drawer")' 8

    Invoke-Js 'window.__smoke.clickText(".filter-bar .btn", "\u5b9e\u65f6\u63a8\u9001")' | Out-Null
    Check-Wait "SSE stream starts" '!!document.querySelector(".filter-bar .btn.polling")' 10
    # 推送接管快照后, 5s 的 REST 轮询必须让位: 包住 fetch 记录一个完整轮询周期
    # 内对 /api/data/latest 的调用 (直接用浏览器侧计数, 避免 CDP 事件排队误差)
    Invoke-Js '(function () { window.__latestCalls = []; window.__sseAt = performance.now(); var orig = window.fetch; window.fetch = function (input) { var u = typeof input === "string" ? input : ((input && input.url) || ""); if (u.indexOf("/api/data/latest") >= 0) window.__latestCalls.push(Math.round(performance.now() - window.__sseAt)); return orig.apply(window, arguments); }; return true; })()' | Out-Null
    Start-Sleep -Milliseconds 5600
    $late = Invoke-Js 'window.__latestCalls.length'
    $lateAt = Invoke-Js 'window.__latestCalls.join(",")'
    Check "SSE replaces REST snapshot polling" ([int]$late -eq 0) ("calls at ms: $lateAt")
    Invoke-Js 'document.querySelector(".filter-bar .btn.polling").click(); true' | Out-Null
    Check-Wait "SSE stream stops" '!document.querySelector(".filter-bar .btn.polling")' 8

    # =======================================================================
    Write-Head "SIMULATOR"
    Invoke-Js 'window.__smoke.nav(2); true' | Out-Null
    Check-Wait "simulator panel opens" '!!document.querySelector(".sim")' 15
    Check-Js "simulator shows cards or empty state" 'document.querySelectorAll(".sim .cards .card").length >= 1 || !!document.querySelector(".sim .empty")'
    Check-Js "simulator config block is present" '!!document.querySelector(".sim details.cfg")'
    Check-Js "register table has no hex column" '(function () { var ths = document.querySelectorAll(".regs thead th"); return ths.length === 0 || (ths.length === 4 && !!document.querySelector(".regs .hex")); })()'

    # =======================================================================
    Write-Head "RESPONSIVE"
    Invoke-Js 'window.__smoke.nav(0); true' | Out-Null
    Start-Sleep -Milliseconds 400
    Set-Viewport 1280 900
    Check-Js "1280: brand subtitle visible" 'getComputedStyle(document.querySelector(".brand-sub")).display !== "none"'
    Check-Js "1280: topbar stays 56px" 'Math.round(document.querySelector(".topbar").getBoundingClientRect().height) === 56'
    Set-Viewport 1024 768
    Check-Js "1024: brand subtitle hidden" 'getComputedStyle(document.querySelector(".brand-sub")).display === "none"'
    Check-Js "1024: status chip hidden" 'getComputedStyle(document.querySelector(".tb-status")).display === "none"'
    Set-Viewport 900 800
    Check-Js "900: brand name hidden" 'getComputedStyle(document.querySelector(".brand-name")).display === "none"'
    Set-Viewport 560 800
    Check-Js "560: brand block hidden" 'getComputedStyle(document.querySelector(".brand")).display === "none"'
    Check-Js "560: no horizontal page overflow" 'document.documentElement.scrollWidth <= window.innerWidth + 1'
    Check-Js "560: nav stays on one row" 'document.querySelector(".nav").getBoundingClientRect().height <= 36'
    Set-Viewport 1440 900

    # =======================================================================
    Write-Head "GLOBAL ACTIONS"
    Invoke-Js 'document.querySelectorAll(".tb-actions .tb-btn")[0].click(); true' | Out-Null
    Check-Wait "hot reload reports success" '!!document.querySelector(".banner.ok")' 20

    # =======================================================================
    Write-Head "CONSOLE / NETWORK"
    Check "no console errors" ($script:consoleErrors.Count -eq 0) (($script:consoleErrors | Select-Object -First 3) -join " | ")
    Check "no console warnings" ($script:consoleWarnings.Count -eq 0) (($script:consoleWarnings | Select-Object -First 3) -join " | ")
    Check "no uncaught JS exceptions" ($script:pageExceptions.Count -eq 0) (($script:pageExceptions | Select-Object -First 3) -join " | ")
    Check "no HTTP 429 (rate limited)" ($script:throttled.Count -eq 0) (($script:throttled | Select-Object -First 3) -join " | ")
    Check "no HTTP 5xx" ($script:serverErrors.Count -eq 0) (($script:serverErrors | Select-Object -First 3) -join " | ")
    Check "no failed network requests" ($script:failedRequests.Count -eq 0) (($script:failedRequests | Select-Object -First 3) -join " | ")
} catch {
    Write-Host ""
    Write-Host ("[SETUP/RUNTIME ERROR] " + $_.Exception.Message) -ForegroundColor Red
    Write-Host ($_.ScriptStackTrace) -ForegroundColor DarkGray
    $script:checks.Add([pscustomobject]@{ Name = "smoke run completed"; Ok = $false })
} finally {
    if ($script:ws) {
        try { $script:ws.Dispose() } catch { }
    }
    if ($KeepAlive) {
        Write-Host ""
        Write-Host ("KEEPALIVE: app {0}  edge cdp {1}  url {2}" -f $ApiPort, $CdpPort, $baseUrl) -ForegroundColor Yellow
    } else {
        Stop-SmokeProcesses
        if ($script:appOut) { Remove-WithRetry $script:appOut }
        if ($script:appErr) { Remove-WithRetry $script:appErr }
        if ($script:browserOut) { Remove-WithRetry $script:browserOut }
        if ($script:browserErr) { Remove-WithRetry $script:browserErr }
    }
}

# ---------------------------------------------------------------- summary

Write-Head "SUMMARY"
$failed = 0
foreach ($c in $script:checks) {
    if (-not $c.Ok) { $failed++ }
}
Write-Host ("{0}/{1} checks passed" -f ($script:checks.Count - $failed), $script:checks.Count)
if ($failed -eq 0) {
    Write-Host "WEBUI SMOKE PASSED" -ForegroundColor Green
    exit 0
}
Write-Host ("WEBUI SMOKE FAILED: {0} check(s)" -f $failed) -ForegroundColor Red
if ($failed -gt 99) { exit 99 }
exit $failed
