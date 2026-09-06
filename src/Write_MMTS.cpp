#include "stdafx.h"
#include "WriteMain.h"
#include <string>
#include <map>
#include <set>
#include <mutex>
#include <vector>
#include <atomic>
#include <tlhelp32.h>

#ifndef PLUGIN_NAME
#define PLUGIN_NAME L"MMTS / TS ハイブリッド保存 PlugIn"
#endif

#ifdef _WIN32
#include <shellapi.h>
#endif

HINSTANCE g_instance = NULL;

typedef BOOL (WINAPI *StartMmtsRecordingFunc)(const wchar_t*, BOOL, DWORD*);
typedef void (WINAPI *StopMmtsRecordingFunc)(DWORD);
typedef BOOL (WINAPI *GetMmtsRecordingStatusFunc)(DWORD, DWORD*, BOOL*, BOOL*);
typedef BOOL (WINAPI *IsMmtsRecordingAvailableFunc)();

struct DanttoMmtsApi {
    HMODULE module = NULL;
    StartMmtsRecordingFunc start = nullptr;
    StopMmtsRecordingFunc stop = nullptr;
    GetMmtsRecordingStatusFunc status = nullptr;
    // MMT/TLV以外のチャンネルも扱うモジュール(BonDriver_Mirakurun等)が
    // 「今MMT/TLVを出力しているか」を答えるためのオプションのexport。
    // 持たないモジュール(dantto4k等のMMT/TLV専用BonDriver)ではnullptr。
    IsMmtsRecordingAvailableFunc available = nullptr;
};

// MMTS保存APIを揃えてエクスポートしているモジュールを全て列挙する
std::vector<DanttoMmtsApi> EnumMmtsProviders()
{
    std::vector<DanttoMmtsApi> providers;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return providers;
    }

    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    if (Module32FirstW(hSnapshot, &me)) {
        do {
            DanttoMmtsApi api;
            api.start = reinterpret_cast<StartMmtsRecordingFunc>(GetProcAddress(me.hModule, "StartMmtsRecording"));
            api.stop = reinterpret_cast<StopMmtsRecordingFunc>(GetProcAddress(me.hModule, "StopMmtsRecording"));
            api.status = reinterpret_cast<GetMmtsRecordingStatusFunc>(GetProcAddress(me.hModule, "GetMmtsRecordingStatus"));
            if (api.start != nullptr && api.stop != nullptr && api.status != nullptr) {
                api.module = me.hModule;
                api.available = reinterpret_cast<IsMmtsRecordingAvailableFunc>(GetProcAddress(me.hModule, "IsMmtsRecordingAvailable"));
                providers.push_back(api);
            }
        } while (Module32NextW(hSnapshot, &me));
    }
    CloseHandle(hSnapshot);
    return providers;
}

// 今まさにMMT/TLVを出力しているモジュールを1つに絞る。
//  - IsMmtsRecordingAvailable()を持つモジュール(BonDriver_Mirakurun等、GR/BS/CSの
//    TSチャンネルも同じDLLで扱うBonDriver)は、TRUEを返したときだけ候補にする。
//  - 持たないモジュール(dantto4k等のMMT/TLV専用BonDriver)は入力が常にMMT/TLVなので
//    常に候補にする(こちらは変更なしでそのまま使える)。
// 候補がちょうど1つのときだけMMTS保存を行う。候補が0個(=通常のTSチャンネルを受信中)
// のときはもちろん、複数見つかった場合(dantto4kがMMT変換を有効にしたままの
// BonDriver_Mirakurunをラップしている等、どのモジュールの出力がEDCBに届いているか
// 確定できない構成)も、TSデータを捨てずに済む.ts保存へフォールバックする。
bool SelectMmtsProvider(DanttoMmtsApi* selected)
{
    std::vector<DanttoMmtsApi> providers = EnumMmtsProviders();

    const DanttoMmtsApi* candidate = nullptr;
    size_t candidateCount = 0;
    for (const DanttoMmtsApi& api : providers) {
        if (api.available != nullptr && api.available() == FALSE) {
            continue;
        }
        candidate = &api;
        ++candidateCount;
    }

    if (candidateCount != 1) {
        return false;
    }
    if (selected != nullptr) {
        *selected = *candidate;
    }
    return true;
}

// 選んだモジュールが録画中にアンロードされないよう参照を取る
// (FreeLibrary()で返すまでこのプラグイン専用の参照カウントが1つ増える)
HMODULE AcquireProviderModule(const DanttoMmtsApi& api)
{
    HMODULE ref = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(api.start), &ref) == FALSE) {
        return NULL;
    }
    return ref;
}

std::wstring MakeMmtsPath(LPCWSTR fileName)
{
    std::wstring path = fileName != nullptr ? fileName : L"";
    size_t slashPos = path.find_last_of(L"\\/");
    size_t dotPos = path.find_last_of(L'.');
    if (dotPos != std::wstring::npos &&
        (slashPos == std::wstring::npos || dotPos > slashPos)) {
        path = path.substr(0, dotPos) + L".mmts";
    } else {
        path += L".mmts";
    }
    return path;
}

std::wstring MakeNumberedPath(const std::wstring& path, int number)
{
    if (number <= 0) {
        return path;
    }

    size_t slashPos = path.find_last_of(L"\\/");
    size_t dotPos = path.find_last_of(L'.');
    std::wstring suffix = L"-(" + std::to_wstring(number) + L")";
    if (dotPos != std::wstring::npos &&
        (slashPos == std::wstring::npos || dotPos > slashPos)) {
        return path.substr(0, dotPos) + suffix + path.substr(dotPos);
    }
    return path + suffix;
}

std::wstring MakeMmtsMapPath(const std::wstring& path)
{
    return path + L"map";
}

bool FileExists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Struct to hold state for either MMTS (dantto4k) or original TS (CWriteMain)
// MMTSとTSのどちらを使うかはStartSave()で決める(CreateCtrl()の時点ではまだ
// 選局が済んでおらず、MMT/TLVを受信中かどうかを判定できないため)。
struct UnifiedInstance {
    bool useMMTS = false;
    bool mmtsStarted = false;
    DWORD mmtsSessionId = 0;
    std::wstring mmtsSavePath;
    // StartSave()で選んだモジュール。以降のStop/Statusは必ずこれを使う
    // (sessionIdはモジュールごとに独立した番号空間なので、呼ぶたびに
    //  モジュールを探し直すと別モジュールの無関係なセッションを操作しうる)。
    DanttoMmtsApi mmtsApi;
    HMODULE mmtsApiModuleRef = NULL;
    // MMTS保存中の可用性再確認用。AddTSBuff()から間引いて問い合わせるための
    // 前回確認時刻と、可用でなくなった時刻。
    // (EDCBのWrite PlugInは_WIN32_WINNT_WS03向けにビルドするためGetTickCount64()を
    //  使えない。GetTickCount()は約49日で一周するが、扱うのは差分だけなので
    //  DWORDの符号なし演算でそのまま正しく求まる)
    std::atomic<DWORD> availCheckTick{ 0 };
    std::atomic<bool> unavailable{ false };
    std::atomic<DWORD> unavailableSinceTick{ 0 };
    std::shared_ptr<CWriteMain> originalInst;
    std::mutex stateMutex;
};

// MMTS保存中の可用性を確認する間隔と、可用でない状態が続いたときに
// 書き込みを失敗扱いにするまでの猶予。
// 猶予はMMT/TLVチャンネル間で選局し直したときに誤検出しないためのもので、
// その間に可用でなくなるのはCloseTuner()の実行中(スレッド終了待ちで最大数秒)
// に限られるため、それより十分長く取る。
static const DWORD MMTS_AVAILABILITY_CHECK_INTERVAL_MS = 1000;
static const DWORD MMTS_AVAILABILITY_GRACE_MS = 10000;

static std::map<DWORD, std::shared_ptr<UnifiedInstance>> g_instances;
static std::set<std::wstring> g_pendingMmtsPaths;
static DWORD g_nextId = 1;
static std::mutex g_mutex;

bool EqualPathNoCase(const std::wstring& left, const std::wstring& right)
{
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

bool IsActiveMmtsPath(const std::wstring& path)
{
    for (const std::wstring& pendingPath : g_pendingMmtsPaths) {
        if (EqualPathNoCase(pendingPath, path)) {
            return true;
        }
    }

    for (const auto& item : g_instances) {
        const std::shared_ptr<UnifiedInstance>& inst = item.second;
        if (!inst || !inst->useMMTS) {
            continue;
        }

        std::lock_guard<std::mutex> stateLock(inst->stateMutex);
        if (inst->mmtsStarted && EqualPathNoCase(inst->mmtsSavePath, path)) {
            return true;
        }
    }
    return false;
}

// MMTS保存中のモジュールがMMT/TLVを出力しなくなっていないか確認する。
// AddTSBuff()から呼ばれるので問い合わせは間引き、また再選局の一瞬で誤検出
// しないよう、可用でない状態が猶予時間続いた場合にだけ失われたと判定する。
// IsMmtsRecordingAvailable()を持たないモジュール(MMT/TLV専用のdantto4k)は
// 入力が常にMMT/TLVなので確認しない。
bool IsMmtsProviderLost(const std::shared_ptr<UnifiedInstance>& inst)
{
    if (inst->mmtsApi.available == nullptr) {
        return false;
    }

    const DWORD now = GetTickCount();
    if (now - inst->availCheckTick.load() >= MMTS_AVAILABILITY_CHECK_INTERVAL_MS) {
        inst->availCheckTick.store(now);
        if (inst->mmtsApi.available() != FALSE) {
            inst->unavailable.store(false);
        } else if (!inst->unavailable.load()) {
            inst->unavailableSinceTick.store(now);
            inst->unavailable.store(true);
        }
    }

    return inst->unavailable.load() &&
        now - inst->unavailableSinceTick.load() >= MMTS_AVAILABILITY_GRACE_MS;
}

// 走っているMMTS保存セッションがあれば止める
// (sessionIdはモジュールごとに独立なので必ずStartSave()で選んだモジュールへ返す)
void StopMmtsSession(const std::shared_ptr<UnifiedInstance>& inst)
{
    bool shouldStop = false;
    {
        std::lock_guard<std::mutex> stateLock(inst->stateMutex);
        shouldStop = inst->mmtsStarted;
        inst->mmtsStarted = false;
    }
    if (shouldStop && inst->mmtsApi.stop != nullptr) {
        inst->mmtsApi.stop(inst->mmtsSessionId);
    }
}

// StartSave()で取ったモジュール参照を返す
void ReleaseMmtsProvider(const std::shared_ptr<UnifiedInstance>& inst)
{
    StopMmtsSession(inst);

    HMODULE ref = NULL;
    {
        std::lock_guard<std::mutex> stateLock(inst->stateMutex);
        ref = inst->mmtsApiModuleRef;
        inst->mmtsApiModuleRef = NULL;
        inst->mmtsApi = DanttoMmtsApi();
        inst->useMMTS = false;
    }
    if (ref != NULL) {
        FreeLibrary(ref);
    }
}

DWORD AllocateInstanceId()
{
    for (DWORD i = 0; i < 0xFFFFFFFF; ++i) {
        DWORD id = g_nextId++;
        if (id == 0) {
            id = g_nextId++;
        }
        if (g_instances.find(id) == g_instances.end()) {
            return id;
        }
    }
    return 0;
}

extern "C" __declspec(dllexport) BOOL WINAPI GetPlugInName(
    WCHAR* name,
    DWORD* nameSize
)
{
    if (nameSize == NULL) {
        return FALSE;
    }
    if (name == NULL) {
        *nameSize = (DWORD)wcslen(PLUGIN_NAME) + 1;
        return TRUE;
    }
    if (*nameSize < (DWORD)wcslen(PLUGIN_NAME) + 1) {
        *nameSize = (DWORD)wcslen(PLUGIN_NAME) + 1;
        return FALSE;
    }
    wcscpy_s(name, *nameSize, PLUGIN_NAME);
    return TRUE;
}

#ifdef _WIN32
#ifdef USE_ONSERVICE
#include "PathUtil.h"
#else
#include "SettingDlg.h"
#include "PathUtil.h"
#endif
#endif

extern "C" __declspec(dllexport) void WINAPI Setting(
    HWND parentWnd
)
{
    if (EnumMmtsProviders().empty() == false) {
        // MMTS保存に対応したBonDriverが同一プロセスにいる。ただしMMTS保存になるのは
        // 実際にMMT/TLVを受信中の録画だけで、通常のTSチャンネルは以下の設定に従って
        // .tsとして保存されるため、設定画面はそのまま開く。
        MessageBoxW(parentWnd,
            L"MMT/TLV(4K/8K)を受信中の録画は、TS書き込みを行わず直接.mmtsを保存します。\n"
            L"それ以外のチャンネルは以下の設定に従って通常の.tsとして保存します。",
            PLUGIN_NAME,
            MB_OK | MB_ICONINFORMATION);
    }

#ifdef _WIN32
#ifdef USE_ONSERVICE
    {
        fs_path iniPath = GetModuleIniPath(g_instance);
        if( GetPrivateProfileToString(L"SET", L"WritePlugin", L"*", iniPath.c_str()) == L"*" ){
            WritePrivateProfileString(L"SET", L"WritePlugin", L";Write_Default.dll", iniPath.c_str());
        }
        ShellExecute(NULL, L"edit", iniPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
    }
#else
    {
        fs_path iniPath = GetModuleIniPath(g_instance);
        wstring size = GetPrivateProfileToString(L"SET", L"Size", L"770048", iniPath.c_str());
        wstring teeCmd = GetPrivateProfileToString(L"SET", L"TeeCmd", L"", iniPath.c_str());
        wstring teeSize = GetPrivateProfileToString(L"SET", L"TeeSize", L"770048", iniPath.c_str());
        wstring teeDelay = GetPrivateProfileToString(L"SET", L"TeeDelay", L"0", iniPath.c_str());
        CSettingDlg dlg;
        if( dlg.CreateSettingDialog(g_instance, parentWnd, size, teeCmd, teeSize, teeDelay) == IDOK ){
            WritePrivateProfileString(L"SET", L"Size", size.c_str(), iniPath.c_str());
            WritePrivateProfileString(L"SET", L"TeeCmd", (teeCmd.find(L'"') == wstring::npos ? teeCmd : L'"' + teeCmd + L'"').c_str(), iniPath.c_str());
            WritePrivateProfileString(L"SET", L"TeeSize", teeSize.c_str(), iniPath.c_str());
            WritePrivateProfileString(L"SET", L"TeeDelay", teeDelay.c_str(), iniPath.c_str());
        }
    }
#endif
#endif
}

// TSフォールバック用のEDCB標準Write PlugIn相当のインスタンスを用意する。
// MMTS保存になる録画では使わないので(USE_ONSERVICEでは下位プラグインDLLの
// ロードまで行うため)、StartSave()でTS保存に決まってから生成する。
bool EnsureOriginalInstance(const std::shared_ptr<UnifiedInstance>& inst)
{
    if (inst->originalInst) {
        return true;
    }

    // original CreateCtrl logic compiled statically
#ifdef USE_ONSERVICE
    fs_path pluginPath;
    fs_path iniPath = GetModuleIniPath(g_instance);
    wstring pluginName = GetPrivateProfileToString(L"SET", L"WritePlugin", L"", iniPath.c_str());
    if( pluginName.empty() == false && pluginName[0] != L';' ){
        pluginPath = GetModulePath(g_instance);
        pluginPath.replace_filename(pluginName);
    }
#else
    fs_path iniPath = GetModuleIniPath(g_instance);
    DWORD buffSize = GetPrivateProfileInt(L"SET", L"Size", 770048, iniPath.c_str());
    DWORD teeSize = 0;
    DWORD teeDelay = 0;
    wstring teeCmd = GetPrivateProfileToString(L"SET", L"TeeCmd", L"", iniPath.c_str());
    if( teeCmd.empty() == false ){
        teeSize = GetPrivateProfileInt(L"SET", L"TeeSize", 770048, iniPath.c_str());
        teeDelay = GetPrivateProfileInt(L"SET", L"TeeDelay", 0, iniPath.c_str());
    }
#endif

    try {
        inst->originalInst = std::make_shared<CWriteMain>();
#ifdef USE_ONSERVICE
        if( pluginPath.empty() == false ){
            inst->originalInst->InitializeDownstreamPlugin(pluginPath.native());
        }
#else
        inst->originalInst->SetBufferSize(buffSize);
        inst->originalInst->SetTeeCommand(teeCmd.c_str(), teeSize, teeDelay);
#endif
    } catch (std::bad_alloc&) {
        inst->originalInst.reset();
        return false;
    }
    return true;
}

extern "C" __declspec(dllexport) BOOL WINAPI CreateCtrl(
    DWORD* id
)
{
    if (id == NULL) {
        return FALSE;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    std::shared_ptr<UnifiedInstance> inst = std::make_shared<UnifiedInstance>();
    if (!inst) {
        return FALSE;
    }

    // MMTS保存かTS保存かはStartSave()まで決められない
    // (EDCBはCreateCtrl()の後にStartSave()を呼ぶが、MMT/TLVを受信中かどうかは
    //  選局後でないと分からず、CreateCtrl()の時点では判定できないため)

    DWORD newId = AllocateInstanceId();
    if (newId == 0) {
        return FALSE;
    }
    *id = newId;
    g_instances[*id] = inst;
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL WINAPI DeleteCtrl(
    DWORD id
)
{
    std::shared_ptr<UnifiedInstance> inst = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_instances.find(id);
        if (it == g_instances.end()) {
            return FALSE;
        }
        inst = it->second;
        g_instances.erase(it);
    }

    ReleaseMmtsProvider(inst);
    return TRUE;
}


extern "C" __declspec(dllexport) BOOL WINAPI StartSave(
    DWORD id,
    LPCWSTR fileName,
    BOOL overWriteFlag,
    ULONGLONG createSize
)
{
    std::shared_ptr<UnifiedInstance> inst = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_instances.find(id);
        if (it == g_instances.end()) {
            return FALSE;
        }
        inst = it->second;
    }

    // 今どちらで保存するかをここで決める。MMT/TLVを出力しているモジュールが
    // 1つに確定したときだけMMTS保存を行い、それ以外(通常のTSチャンネル受信中など)は
    // 従来のTS保存にフォールバックする。
    // 前回のStartSave()で取ったモジュール参照はここで返す
    // (EDCBは空き容量が尽きたときなどに同じctrlへStartSave()を再度呼ぶ)。
    ReleaseMmtsProvider(inst);

    DanttoMmtsApi selectedApi;
    if (SelectMmtsProvider(&selectedApi)) {
        HMODULE moduleRef = AcquireProviderModule(selectedApi);
        if (moduleRef != NULL) {
            std::lock_guard<std::mutex> stateLock(inst->stateMutex);
            inst->mmtsApi = selectedApi;
            inst->mmtsApiModuleRef = moduleRef;
            inst->useMMTS = true;
        }
    }

    if (inst->useMMTS) {
        if (fileName == NULL) {
            ReleaseMmtsProvider(inst);
            return FALSE;
        }

        std::wstring path = MakeMmtsPath(fileName);
        const DanttoMmtsApi& api = inst->mmtsApi;

        DWORD sessionId = 0;
        BOOL started = FALSE;
        for (int i = 0; i < 1000; ++i) {
            std::wstring candidate = MakeNumberedPath(path, i);
            bool reserved = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (!IsActiveMmtsPath(candidate)) {
                    g_pendingMmtsPaths.insert(candidate);
                    reserved = true;
                }
            }
            if (!reserved) {
                continue;
            }
            if (!overWriteFlag && (FileExists(candidate) || FileExists(MakeMmtsMapPath(candidate)))) {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_pendingMmtsPaths.erase(candidate);
                continue;
            }

            started = api.start(candidate.c_str(), overWriteFlag, &sessionId);
            if (started) {
                path = candidate;
                inst->availCheckTick.store(GetTickCount());
                inst->unavailable.store(false);
                std::lock_guard<std::mutex> lock(g_mutex);
                {
                    std::lock_guard<std::mutex> stateLock(inst->stateMutex);
                    inst->mmtsSavePath = path;
                    inst->mmtsSessionId = sessionId;
                    inst->mmtsStarted = true;
                }
                g_pendingMmtsPaths.erase(candidate);
                break;
            }
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_pendingMmtsPaths.erase(candidate);
            }
            if (overWriteFlag && i == 0) {
                continue;
            }
            if (!overWriteFlag && (FileExists(candidate) || FileExists(MakeMmtsMapPath(candidate)))) {
                continue;
            }
            break;
        }
        if (!started) {
            // MMT/TLVは受信できているのに保存を開始できなかった(書き込み先の
            // エラー等)。ここでTS保存へ逃がすとMPEG2-TSに変換済みの映像だけが
            // 残ってしまうので、EDCBには失敗として返す。
            ReleaseMmtsProvider(inst);
            return FALSE;
        }
        return TRUE;
    } else {
        if (EnsureOriginalInstance(inst) == false) {
            return FALSE;
        }
        return inst->originalInst->Start(fileName, overWriteFlag, createSize);
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI StopSave(
    DWORD id
)
{
    std::shared_ptr<UnifiedInstance> inst = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_instances.find(id);
        if (it == g_instances.end()) {
            return FALSE;
        }
        inst = it->second;
    }

    if (inst->useMMTS) {
        bool shouldStop = false;
        {
            std::lock_guard<std::mutex> stateLock(inst->stateMutex);
            shouldStop = inst->mmtsStarted;
            inst->mmtsStarted = false;
        }
        if (shouldStop) {
            if (inst->mmtsApi.stop == nullptr) {
                return FALSE;
            }
            // モジュール参照はDeleteCtrl()か次のStartSave()まで持ったままにする
            // (停止後にGetSaveFilePath()を呼ばれても保存先を返せるようにするため)
            inst->mmtsApi.stop(inst->mmtsSessionId);
        }
        return TRUE;
    } else {
        if (inst->originalInst) {
            return inst->originalInst->Stop();
        }
        return FALSE;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI GetSaveFilePath(
    DWORD id,
    WCHAR* filePath,
    DWORD* filePathSize
)
{
    if (filePathSize == NULL) {
        return FALSE;
    }

    std::shared_ptr<UnifiedInstance> inst = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_instances.find(id);
        if (it == g_instances.end()) {
            return FALSE;
        }
        inst = it->second;
    }

    if (inst->useMMTS) {
        std::wstring path;
        {
            std::lock_guard<std::mutex> stateLock(inst->stateMutex);
            path = inst->mmtsSavePath;
        }
        if (filePath == NULL) {
            *filePathSize = (DWORD)path.size() + 1;
            return TRUE;
        }

        if (*filePathSize < (DWORD)path.size() + 1) {
            *filePathSize = (DWORD)path.size() + 1;
            return FALSE;
        }

        wcscpy_s(filePath, *filePathSize, path.c_str());
        return TRUE;
    } else {
        if (inst->originalInst) {
            std::wstring originalPath = inst->originalInst->GetSavePath();
            if (filePath == NULL) {
                *filePathSize = (DWORD)originalPath.size() + 1;
                return TRUE;
            }

            if (*filePathSize < (DWORD)originalPath.size() + 1) {
                *filePathSize = (DWORD)originalPath.size() + 1;
                return FALSE;
            }

            wcscpy_s(filePath, *filePathSize, originalPath.c_str());
            return TRUE;
        }
        return FALSE;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI AddTSBuff(
    DWORD id,
    BYTE* data,
    DWORD size,
    DWORD* writeSize
)
{
    std::shared_ptr<UnifiedInstance> inst = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_instances.find(id);
        if (it == g_instances.end()) {
            return FALSE;
        }
        inst = it->second;
    }

    if (inst->useMMTS) {
        bool started = false;
        DWORD sessionId = 0;
        {
            std::lock_guard<std::mutex> stateLock(inst->stateMutex);
            started = inst->mmtsStarted;
            sessionId = inst->mmtsSessionId;
        }
        if (!started) {
            if (writeSize != NULL) {
                *writeSize = 0;
            }
            return FALSE;
        }
        if (inst->mmtsApi.status == nullptr) {
            if (writeSize != NULL) {
                *writeSize = 0;
            }
            return FALSE;
        }
        BOOL failed = FALSE;
        if (inst->mmtsApi.status(sessionId, nullptr, &failed, nullptr) == FALSE || failed) {
            if (writeSize != NULL) {
                *writeSize = 0;
            }
            return FALSE;
        }
        // 保存を開始した後でMMT/TLVでないチャンネルに変わっていないか確認する。
        // EDCBは録画中に選局しないが、TVTest(LibISDBのEDCBPluginWriter経由)は
        // 録画中でもチャンネルを変えられる。変わってもMMTS保存側は失敗を報告
        // しないため、.mmtsが伸びないまま録画が続いてしまう。
        if (IsMmtsProviderLost(inst)) {
            if (writeSize != NULL) {
                *writeSize = 0;
            }
            return FALSE;
        }
        if (writeSize != NULL) {
            *writeSize = size;
        }
        return TRUE;
    } else {
        if (inst->originalInst) {
            return inst->originalInst->Write(data, size, writeSize);
        }
        return FALSE;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        g_instance = hModule;
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
