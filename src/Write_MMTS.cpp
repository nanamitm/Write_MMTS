#include "stdafx.h"
#include "WriteMain.h"
#include <string>
#include <map>
#include <mutex>
#include <tlhelp32.h>

#ifndef PLUGIN_NAME
#define PLUGIN_NAME L"MMTS / TS ハイブリッド保存 PlugIn"
#endif

#ifdef _WIN32
#include <shellapi.h>
#endif

HINSTANCE g_instance = NULL;

typedef BOOL (WINAPI *StartMmtsSaveFunc)(const wchar_t*, BOOL);
typedef void (WINAPI *StopMmtsSaveFunc)();

struct DanttoMmtsApi {
    HMODULE module = NULL;
    StartMmtsSaveFunc start = nullptr;
    StopMmtsSaveFunc stop = nullptr;
};

DanttoMmtsApi FindDanttoMmtsApi()
{
    DanttoMmtsApi api;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return api;
    }

    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    if (Module32FirstW(hSnapshot, &me)) {
        do {
            auto start = reinterpret_cast<StartMmtsSaveFunc>(GetProcAddress(me.hModule, "StartMmtsSave"));
            auto stop = reinterpret_cast<StopMmtsSaveFunc>(GetProcAddress(me.hModule, "StopMmtsSave"));
            if (start != nullptr && stop != nullptr) {
                api.module = me.hModule;
                api.start = start;
                api.stop = stop;
                break;
            }
        } while (Module32NextW(hSnapshot, &me));
    }
    CloseHandle(hSnapshot);
    return api;
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

// Struct to hold state for either MMTS (dantto4k) or original TS (CWriteMain)
struct UnifiedInstance {
    bool useMMTS = false;
    bool mmtsStarted = false;
    std::wstring mmtsSavePath;
    std::shared_ptr<CWriteMain> originalInst;
    std::mutex stateMutex;
};

static std::map<DWORD, std::shared_ptr<UnifiedInstance>> g_instances;
static DWORD g_nextId = 1;
static std::mutex g_mutex;

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
    DanttoMmtsApi api = FindDanttoMmtsApi();

    if (api.module != NULL) {
        MessageBoxW(parentWnd, 
            L"dantto4kと連携して、TS書き込みを無効化し、直接.mmtsを保存します。\n設定項目はありません。", 
            PLUGIN_NAME, 
            MB_OK | MB_ICONINFORMATION);
        return;
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

    DanttoMmtsApi api = FindDanttoMmtsApi();

    if (api.module != NULL) {
        // Modified dantto4k is loaded! Use MMTS recording mode
        inst->useMMTS = true;
    } else {
        // dantto4k is NOT loaded or unmodified. Fallback to statically compiled TS plugin!
        inst->useMMTS = false;
        
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
            return FALSE;
        }
    }

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

    if (inst->useMMTS) {
        bool shouldStop = false;
        {
            std::lock_guard<std::mutex> stateLock(inst->stateMutex);
            shouldStop = inst->mmtsStarted;
            inst->mmtsStarted = false;
        }
        if (shouldStop) {
            DanttoMmtsApi api = FindDanttoMmtsApi();
            if (api.stop != nullptr) {
                api.stop();
            }
        }
    }
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

    if (inst->useMMTS) {
        if (fileName == NULL) {
            return FALSE;
        }

        std::wstring path = MakeMmtsPath(fileName);
        DanttoMmtsApi api = FindDanttoMmtsApi();
        if (api.start == nullptr || api.stop == nullptr) {
            return FALSE;
        }

        if (!overWriteFlag) {
            DWORD attr = GetFileAttributesW(path.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES) {
                return FALSE;
            }
        }

        BOOL started = api.start(path.c_str(), overWriteFlag);
        if (!started) {
            return FALSE;
        }

        {
            std::lock_guard<std::mutex> stateLock(inst->stateMutex);
            inst->mmtsSavePath = path;
            inst->mmtsStarted = true;
        }
        return TRUE;
    } else {
        if (inst->originalInst) {
            return inst->originalInst->Start(fileName, overWriteFlag, createSize);
        }
        return FALSE;
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
            DanttoMmtsApi api = FindDanttoMmtsApi();
            if (api.stop == nullptr) {
                return FALSE;
            }
            api.stop();
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
        {
            std::lock_guard<std::mutex> stateLock(inst->stateMutex);
            started = inst->mmtsStarted;
        }
        if (!started) {
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
