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

MMTS 保存と TS 保存のどちらを使うかは、録画ごとに `StartSave()` の時点で決めます
(選局が済むまで MMT/TLV を受信中かどうか判定できないため)。

- MMT/TLV を出力しているモジュールが 1 つに確定する場合
  - EDCB が指定した `.ts` 保存先を `.mmts` に置き換えます。
  - TS 書き込みは行わず、そのモジュール側で MMTS を直接保存します。
  - MMTS は受信チャンネルの stream をそのまま保存します。
- それ以外の場合
  - 内蔵した EDCB 標準 Write PlugIn の処理にフォールバックし、通常の `.ts` 録画を行います。

モジュールの絞り込みは次の規則で行います。

- `IsMmtsRecordingAvailable()` を持つモジュール(BonDriver_Mirakurun のように
  GR/BS/CS の TS チャンネルも同じ DLL で扱う BonDriver)は、`TRUE` を返したときだけ候補にします。
- `IsMmtsRecordingAvailable()` を持たないモジュール(dantto4k のような MMT/TLV 専用 BonDriver)は、
  入力が常に MMT/TLV なので常に候補にします。
- 候補がちょうど 1 つのときだけ MMTS 保存を行います。候補が 0 個(通常の TS チャンネルを受信中)
  のときはもちろん、複数見つかった場合(dantto4k が MMT 変換を有効にしたままの
  BonDriver_Mirakurun をラップしている等、どのモジュールの出力が EDCB に届いているか
  確定できない構成)も、TS データを捨てずに済む `.ts` 保存へフォールバックします。

選んだモジュールは録画中キャッシュし、`StopSave()`/`AddTSBuff()` は必ずそのモジュールへ呼びます
(`sessionId` はモジュールごとに独立した番号空間のため)。

出力 DLL は 2 種類です。

- `Write_MMTS_OneService.dll`: TS フォールバック時は `Write_OneService` 相当。MMTS 保存時はチャンネル stream を保存します。
- `Write_MMTS_Default.dll`: TS フォールバック時は `Write_Default` 相当。MMTS 保存時はチャンネル stream を保存します。

## 必要条件

本プラグインで MMTS 保存を行うには、相手側に以下の export が必要です。

```cpp
extern "C" __declspec(dllexport) BOOL WINAPI StartMmtsRecording(const wchar_t* path, BOOL overwrite, DWORD* sessionId);
extern "C" __declspec(dllexport) void WINAPI StopMmtsRecording(DWORD sessionId);
extern "C" __declspec(dllexport) BOOL WINAPI GetMmtsRecordingStatus(DWORD sessionId, DWORD* actualMode, BOOL* failed, BOOL* fallbackUsed);
```

MMT/TLV 以外のチャンネルも同じ DLL で扱う BonDriver は、加えて以下の export が必要です。

```cpp
extern "C" __declspec(dllexport) BOOL WINAPI IsMmtsRecordingAvailable();
```

この export が無いモジュールは「入力が常に MMT/TLV である」とみなします。
dantto4k のような MMT/TLV 専用 BonDriver はこの前提を満たすため、追加の実装は不要です。
将来 TS を素通しするような入力モードを持たせる場合は、この export を実装してください。

`StartMmtsRecording()` が `FALSE` を返した場合、Write PlugIn 側の `StartSave()` も失敗扱いにします。
MMTS 保存が開始できない状態で TS データだけを破棄しないためです。

`GetMmtsRecordingStatus()` で失敗が通知された場合、`AddTSBuff()` も失敗扱いにします。
dantto4k 側で復号に失敗した場合は raw fallback が使われることがあります。

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
