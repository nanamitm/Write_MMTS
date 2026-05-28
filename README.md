# Write_MMTS

EDCB (EpgDataCap_Bon) 用の Write PlugIn です。
通常の MPEG-2 TS 録画と、dantto4k 連携による MMTS 録画を自動で切り替えます。

## フォルダ構成

```text
Write_MMTS/
  src/                         Write PlugIn 本体
  scripts/                     ビルド補助スクリプト
  thirdparty/EDCB/             ビルドに必要な EDCB ソースの最小コピー
  CMakeLists.txt
  README.md
  THIRD_PARTY_NOTICES.md
```

## 動作

- dantto4k が同一プロセスにロードされ、MMTS 保存用 export が見つかる場合
  - EDCB が指定した `.ts` 保存先を `.mmts` に置き換えます。
  - TS 書き込みは行わず、dantto4k 側で MMTS を直接保存します。
- dantto4k が未ロード、または必要な export がない場合
  - 内蔵した EDCB 標準 Write PlugIn の処理にフォールバックし、通常の `.ts` 録画を行います。

出力 DLL は 2 種類です。

- `Write_MMTS_OneService.dll`: `Write_OneService` 相当。指定サービスのみ保存します。
- `Write_MMTS_Default.dll`: `Write_Default` 相当。全 TS を保存します。

## 必要条件

本プラグインで MMTS 保存を行うには、dantto4k 側に以下の export が必要です。

```cpp
extern "C" __declspec(dllexport) BOOL WINAPI StartMmtsSave(const wchar_t* path, BOOL overwrite);
extern "C" __declspec(dllexport) void WINAPI StopMmtsSave();
```

`StartMmtsSave()` が `FALSE` を返した場合、Write PlugIn 側の `StartSave()` も失敗扱いにします。
MMTS 保存が開始できない状態で TS データだけを破棄しないためです。

## ビルド

必要な EDCB ソースは `thirdparty/EDCB` に同梱しているため、別途 EDCB ソースツリーを配置する必要はありません。

```powershell
.\scripts\build.ps1 -Configuration Release
```

CMake を直接実行する場合:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

生成物:

- `build/bin/Release/Write_MMTS_OneService.dll`
- `build/bin/Release/Write_MMTS_Default.dll`

## 導入

1. 使用する DLL を EDCB の実行フォルダにコピーします。
2. `EpgDataCap_Bon.exe` の基本設定で Write PlugIn として選択します。
3. dantto4k 連携が有効な環境では `.mmts`、それ以外では通常の `.ts` として保存されます。

オリジナルの `Write_OneService.dll` / `Write_Default.dll` を同梱する必要はありません。

## EDCB ソースの更新

vendored EDCB ソースを更新する場合は、コピー元 EDCB のパスを指定して同期します。

```powershell
.\scripts\sync-edcb-sources.ps1 -EdcbRoot F:\path\to\EDCB
```
