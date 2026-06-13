# 建置流程

本文件是本專案的標準建置流程。安裝建置工具、執行建置及驗證產出時，皆應遵循本文件。

## 支援環境

- Windows 11
- Visual Studio Build Tools 2022
- MSVC v143 x64/x86 Build Tools
- Windows 11 SDK 10.0.26100.0 或更新版本

不需要安裝完整的 Visual Studio IDE。

## 必要工具

建置流程會使用下列工具：

- `cl.exe`：編譯 C 原始碼
- `rc.exe`：編譯 Windows 圖示與資源
- `fxc.exe`：編譯 HLSL 著色器
- `link.exe`：連結 Windows 執行檔

`build.cmd` 會透過 Visual Studio Installer 的 `vswhere.exe` 尋找包含
`Microsoft.VisualStudio.Component.VC.Tools.x86.x64` 元件的安裝，並自動載入
Visual Studio 開發人員命令列環境。

## 安裝建置工具

從 [Microsoft C++ Build Tools](https://visualstudio.microsoft.com/downloads/) 下載
Visual Studio Build Tools 2022。

在安裝程式中選擇 **Desktop development with C++**，並保留下列元件：

- MSVC v143 x64/x86 build tools
- Windows 11 SDK
- C++ Build Tools 核心元件

一般發行版本建置不需要下列選用元件：

- CMake
- ATL、MFC
- AddressSanitizer
- 測試工具
- Clang
- vcpkg

`x64 debug` 建置會使用 AddressSanitizer；只有需要此建置模式時，才需另外安裝
**C++ AddressSanitizer** 元件。

安裝完成後，請關閉並重新開啟終端機。

## 標準建置

所有指令都必須從專案根目錄執行：

```powershell
cd D:\Project\wcap
```

正式 x64 建置是本專案的標準驗證方式：

```powershell
cmd /c build.cmd x64
```

執行建置前，必須先從系統匣結束既有的 `wcap-x64.exe`。Windows 無法覆寫正在執行的
程式；若目標程式仍在執行，`build.cmd` 會在編譯前顯示錯誤並停止。

成功後會在專案根目錄產生：

```text
wcap-x64.exe
```

建置過程產生的 `shaders` 目錄及中間檔案不納入版本控制。

## 其他建置模式

依目前電腦的處理器架構建置正式版本：

```powershell
cmd /c build.cmd
```

建置 x64 偵錯版本：

```powershell
cmd /c build.cmd x64 debug
```

建置 ARM64 正式版本：

```powershell
cmd /c build.cmd arm64
```

ARM64 建置需要對應的 MSVC ARM64 工具元件。除非要交付 ARM64 版本，否則不列入
標準驗證。

## 驗證

每次修改建置流程或 C 原始碼後，至少執行：

```powershell
cmd /c build.cmd x64
```

建置成功必須同時符合：

1. 指令結束碼為 `0`。
2. `wcap-x64.exe` 已產生。
3. 編譯輸出沒有警告或錯誤；專案使用 `/WX`，警告會直接視為錯誤。
4. `git status --short` 只顯示預期的原始碼或文件變更。

原始碼包含台灣正體中文文字，因此 `build.cmd` 必須保留 MSVC 的 `/utf-8` 選項。

## 常見問題

### Visual Studio installation not found

確認 Visual Studio Build Tools 2022 已安裝
`Microsoft.VisualStudio.Component.VC.Tools.x86.x64`。不要只安裝 Visual Studio Installer
或 MSVC 可轉散發套件（Redistributable）。

### wcap-x64.exe is running

從系統匣結束目前執行中的 wcap，再重新執行建置。建置腳本不會自動終止程式，以免中斷
正在進行的錄影。

### 找不到 rc.exe 或 fxc.exe

Windows 11 SDK 未完整安裝。請從 Visual Studio Installer 修改 Build Tools，加入 Windows
11 SDK。

### C4819 或字串常值包含換行

這通常表示原始碼沒有以 UTF-8 編譯。確認 `build.cmd` 呼叫 `cl.exe` 時仍包含
`/utf-8`，不要移除此選項。

### x64 debug 建置找不到 AddressSanitizer

從 Visual Studio Installer 加裝 **C++ AddressSanitizer**，或改用標準的 x64 正式版本
建置。
