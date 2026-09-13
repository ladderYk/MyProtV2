# 构建脚本 — 规避 Vite/Rollup 无法处理含 '#' 路径的问题 (本仓库位于 c#\MyProt-master)
# 原理: 将 webui 源码同步到无 '#' 的临时目录执行 yarn install + yarn build,
#       构建完成后把 dist/ 产物回拷到 webui/dist。
$src = 'D:\workSpaces\c++\MyProt-master\webui'
# 注: 沙箱子进程中 $env:TEMP 可能为空, 用 .NET API 获取临时目录
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) 'myprot-webui-build'
Write-Host "构建临时目录: $tmp"

# 1. 同步源码到临时构建目录 (/MIR 保持一致; 排除依赖与产物)
robocopy $src $tmp /MIR /NFL /NDL /NJH /NJS /XD dist node_modules | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy 同步失败: $LASTEXITCODE" }

# 2. 安装依赖并构建
Push-Location $tmp
try {
    yarn install --frozen-lockfile
    if ($LASTEXITCODE -ne 0) { throw "yarn install 失败" }
    yarn build
    if ($LASTEXITCODE -ne 0) { throw "vite build 失败" }
}
finally {
    Pop-Location
}

# 3. 回拷构建产物到真实目录
robocopy (Join-Path $tmp 'dist') (Join-Path $src 'dist') /MIR /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy 回拷失败: $LASTEXITCODE" }

Write-Host "构建完成 → $src\dist"
