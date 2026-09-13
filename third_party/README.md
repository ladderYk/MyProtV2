# third_party — 第三方依赖 (vendor 模式)

所有第三方依赖以源码 vendor 方式内置于 `third_party/` 目录，不使用 vcpkg 或系统包管理器。
版本锁定以兼容 VS2015 (MSVC v140, C++11) 为准 (ADR-0010 §4)。

## 依赖清单

| 库 | 版本 | 用途 | 说明 |
|----|------|------|------|
| [asio](https://think-async.com/Asio/) | 1.20.0 | 异步 I/O | standalone 模式 (`ASIO_STANDALONE`), 不依赖 Boost |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.7.3 | JSON 解析 | 最后一个官方支持 VS2015 的版本 |
| [GoogleTest](https://github.com/google/googletest) | 1.8.x | 单元测试 | VS2015 兼容的最后一代 |
| [OpenSSL](https://www.openssl.org/) | 1.1.1 (预编译) | TLS | v1 TlsChannel 仍为 stub（基于 `asio::ssl::stream<asio::ip::tcp::socket>`，未实现 .cpp）；vcxproj 保留 include path 与 lib 链接以备 v1.5+ 启用 |

## 目录结构

```
third_party/
├── asio/              → 解压 asio 源码到 include/
├── nlohmann_json/     → 解压 single-include 到 include/
├── gtest/             → 预编译 lib (lib/Win32, lib/x64) + include/
└── openssl/           → 预编译二进制 (include/, lib/Win32/, lib/x64/)
```

## 获取方式

运行 `scripts/setup.bat` 自动下载并解压依赖到对应目录。
或手动下载后按上述目录结构放置。

## 注意事项

- asio 使用 standalone 模式, 定义 `ASIO_STANDALONE` 和 `ASIO_NO_DEPRECATED`
- OpenSSL 1.1.1 需要预编译的 Windows 二进制 (推荐从 https://slproweb.com/products/Win32-OpenSSL.html 获取);v1 TlsChannel 仍为 stub 不真正调用，链接仅作 v1.5+ 启用准备
- GoogleTest 需预编译为静态库 (gtest.lib, gtest_main.lib)
