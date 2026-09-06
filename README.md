# Write_MMTS

EDCB (EpgDataCap_Bon) 用の Write PlugIn です。
通常の MPEG-2 TS 録画と、MMTS 保存に対応した BonDriver 連携による MMTS 録画を、
録画ごとに自動で切り替えます。

EDCB の Write PlugIn を読み込めるホストであれば EDCB 以外からも使えます
(TVTest は LibISDB の `EDCBPluginWriter` 経由で対応しています)。
ただし MMTS 保存にはホスト側の一部機能が効かないため、[MMTS 保存時の制限](#mmts-保存時の制限)
を確認してください。

## フォルダ構成

```text
Write_MMTS/
  src/                         Write PlugIn 本体
  scripts/                     ビルド補助スクリプト
  thirdparty/EDCB/             ビルドに必要な EDCB ソースの最小コピー
  .github/                     GitHub Actions のビルドワークフロー
  CMakeLists.txt
  LICENSE
  README.md
  THIRD_PARTY_NOTICES.md
```

## 動作

MMTS 保存と TS 保存のどちらを使うかは、録画ごとに `StartSave()` の時点で決めます
(選局が済むまで MMT/TLV を受信中かどうか判定できないため)。

- MMT/TLV を出力しているモジュールが 1 つに確定する場合
  - ホストが指定した `.ts` 保存先を `.mmts` に置き換えます。
  - TS 書き込みは行わず、そのモジュール側で MMTS を直接保存します。
  - MMTS は受信チャンネルの stream をそのまま保存します。
  - 索引ファイル `.mmtsmap` もモジュール側が同じ場所に作ります。
    上書きしない設定のときは、この 2 つのどちらかが既にあれば別の名前に採番します。
- それ以外の場合
  - 内蔵した EDCB 標準 Write PlugIn の処理にフォールバックし、通常の `.ts` 録画を行います。

モジュールの絞り込みは次の規則で行います。

- `IsMmtsRecordingAvailable()` を持つモジュール(BonDriver_Mirakurun のように
  GR/BS/CS の TS チャンネルも同じ DLL で扱う BonDriver)は、`TRUE` を返したときだけ候補にします。
- `IsMmtsRecordingAvailable()` を持たないモジュール(dantto4k のような MMT/TLV 専用 BonDriver)は、
  入力が常に MMT/TLV なので常に候補にします。
- 候補がちょうど 1 つのときだけ MMTS 保存を行います。候補が 0 個(通常の TS チャンネルを受信中)
  のときはもちろん、複数見つかった場合(dantto4k が MMT 変換を有効にしたままの
  BonDriver_Mirakurun をラップしている等、どのモジュールの出力がホストに届いているか
  確定できない構成)も、TS データを捨てずに済む `.ts` 保存へフォールバックします。

選んだモジュールは録画中キャッシュし、`StopSave()`/`AddTSBuff()` は必ずそのモジュールへ呼びます
(`sessionId` はモジュールごとに独立した番号空間のため)。

## MMTS 保存時の制限

MMTS 保存は「保存を開始した時点から、モジュールが受信している channel の stream をそのまま書く」
仕組みで、Write PlugIn に渡される TS を一切使いません。そのため、渡された TS を加工・制御する
種類のホスト側機能は MMTS 保存には効きません。EDCB (EpgDataCap_Bon) での録画では問題になりませんが、
TVTest から LibISDB の `EDCBPluginWriter` 経由で使う場合は以下に注意してください。

- **録画中のチャンネル変更**: MMTS 保存中に MMT/TLV でないチャンネルへ変えると、
  そのチャンネルの内容は記録されません。この場合は `AddTSBuff()` を失敗扱いにして、
  ホストに書き込みエラーを通知します(MMT/TLV チャンネル間で選局し直しただけのときに
  誤検出しないよう、10 秒の猶予を置いてから判定します)。
  ただし**ホストが録画を止めるとは限りません**。TVTest はエラーメッセージを一度出すだけで
  録画を続けるため(ディスクフル時と同じ挙動です)、録画時間のカウンタは進み続けます。
  MMTS 保存自体は止めていないので、MMT/TLV のチャンネルへ戻せば同じ `.mmts` の続きに
  記録が再開します。
- **録画の一時停止**: ホストが TS を渡さなくなるだけなので、MMTS 保存は止まりません。
  一時停止した区間も `.mmts` に記録されます。
- **さかのぼり録画(タイムシフト)**: 保存開始より前の分は記録されません。
- **保存するサービス/ストリームの絞り込み**: 受信 channel の stream をそのまま保存するため、
  「現在のサービスのみ保存」等の設定は無視されます。`Write_MMTS_OneService.dll` を使った場合も、
  サービスの絞り込みが効くのは `.ts` へフォールバックしたときだけです。

## 出力 DLL

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

選局中のチャンネルが MMT/TLV で、`StartMmtsRecording()` で開始した保存へデータを流せる状態なら
`TRUE` を返してください。**録画中に `AddTSBuff()` の中からも呼ばれるため、ブロックせずに即座に
返す必要があります**(選局処理と同じロックを取ると、選局のたびにホストの書き込みスレッドを
止めてしまいます)。選局し直している最中に一時的に `FALSE` になるのは構いません。

この export が無いモジュールは「入力が常に MMT/TLV である」とみなします。
dantto4k のような MMT/TLV 専用 BonDriver はこの前提を満たすため、追加の実装は不要です。
将来 TS を素通しするような入力モードを持たせる場合は、この export を実装してください。

`StartMmtsRecording()` が `FALSE` を返した場合、Write PlugIn 側の `StartSave()` も失敗扱いにします
(`.ts` へはフォールバックしません)。ここに来るのは MMT/TLV を受信中だと確認できた後なので、
`.ts` に逃がすと MPEG-2 TS に変換済みの映像だけが残ってしまうためです。

`GetMmtsRecordingStatus()` の `failed` が `TRUE` になった場合は、`AddTSBuff()` も失敗扱いにします。
`fallbackUsed`(復号に失敗して raw のまま保存された)は失敗扱いにしません。保存自体は続いており、
データが失われているわけではないためです。

## ビルド

必要な EDCB ソースは `thirdparty/EDCB` に同梱しているため、別途 EDCB ソースツリーを配置する必要はありません。

```powershell
.\scripts\build.ps1 -Configuration Release
```

generator の既定は `Visual Studio 17 2022` です。別のバージョンの Visual Studio を使う場合は
`-Generator` で指定してください(例: `-Generator "Visual Studio 18 2026"`)。
使用する CMake がその generator を知らないと configure に失敗するため、その場合は
Visual Studio 同梱の `cmake.exe`
(`<VS のインストール先>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`)
を PATH に通すか、下記のように直接実行してください。

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
3. MMT/TLV(4K/8K)を受信中のチャンネルでは `.mmts`、それ以外では通常の `.ts` として保存されます。

オリジナルの `Write_OneService.dll` / `Write_Default.dll` を同梱する必要はありません。

### 自動削除を使う場合の設定 (EpgTimerSrv)

EpgTimerSrv の自動削除(`AutoDel`)は、録画ファイルを消すときに `[DEL_EXT]` に並べた拡張子へ
差し替えたファイルも一緒に消します。既定値は `.ts.err` と `.ts.program.txt` なので、
`.mmts` として保存した録画では以下が消えずに残ります。

- `xxx.mmts.program.txt` (番組情報ファイル)
- `xxx.mmtsmap` (MMTS の索引ファイル)

`EpgTimerSrv.ini` に以下を追記すると一緒に削除されます。`Count` を書くと既定値は使われなく
なるため、`.ts` 用の 2 つも明示的に並べる必要があります。

```ini
[DEL_EXT]
Count=4
0=.ts.err
1=.ts.program.txt
2=.mmts.program.txt
3=.mmtsmap
```

自動削除を使っていない場合は設定不要です。

## EDCB ソースの更新

vendored EDCB ソースを更新する場合は、コピー元 EDCB のパスを指定して同期します。

```powershell
.\scripts\sync-edcb-sources.ps1 -EdcbRoot F:\path\to\EDCB
```
