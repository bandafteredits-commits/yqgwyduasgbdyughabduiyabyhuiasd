// cpoldcrit_inject.exe - loads cpoldcrit.dll into Minecraft Bedrock (Minecraft.Windows.exe).
// Exit code: 0 = injected (or already loaded), 1 = Minecraft not running, 2 = failed
// (including cpoldcrit.dll missing). Run by the "Old sword crit" button in HTMI.proxx.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <aclapi.h>
#include <sddl.h>
#include <string>
#pragma comment(lib, "advapi32.lib")

static DWORD findPid() {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof(pe) }; DWORD pid = 0;
    for (BOOL ok = Process32FirstW(s, &pe); ok; ok = Process32NextW(s, &pe))
        if (!_wcsicmp(pe.szExeFile, L"Minecraft.Windows.exe")) { pid = pe.th32ProcessID; break; }
    CloseHandle(s); return pid;
}
static bool hasModule(DWORD pid, const wchar_t* name) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (s == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me = { sizeof(me) }; bool found = false;
    for (BOOL ok = Module32FirstW(s, &me); ok; ok = Module32NextW(s, &me))
        if (!_wcsicmp(me.szModule, name)) { found = true; break; }
    CloseHandle(s); return found;
}
// Minecraft is a packaged app: it can only load DLLs that "ALL APPLICATION PACKAGES" may read/execute.
static void allowAppPackages(const std::wstring& path) {
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-15-2-1", &sid)) return;
    EXPLICIT_ACCESSW ea = {};
    ea.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    ea.grfAccessMode = GRANT_ACCESS; ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID; ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = (LPWSTR)sid;
    PACL oldAcl = nullptr, newAcl = nullptr; PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &oldAcl, nullptr, &sd) == ERROR_SUCCESS) {
        if (SetEntriesInAclW(1, &ea, oldAcl, &newAcl) == ERROR_SUCCESS) {
            SetNamedSecurityInfoW((LPWSTR)path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, newAcl, nullptr);
            LocalFree(newAcl);
        }
        LocalFree(sd);
    }
    LocalFree(sid);
}

int wmain() {
    DWORD pid = findPid();
    if (!pid) return 1;
    wchar_t me[MAX_PATH]; GetModuleFileNameW(nullptr, me, MAX_PATH);
    std::wstring dll = me; dll.resize(dll.find_last_of(L'\\') + 1); dll += L"cpoldcrit.dll";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) return 2;
    CreateDirectoryW(L"C:\\Users\\Public\\CheesyProxxy", nullptr);
    if (hasModule(pid, L"cpoldcrit.dll")) return 0;           // already loaded: the cfg file does the rest
    allowAppPackages(dll);

    HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                           PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!p) return 2;
    SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(p, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    int rc = 2;
    if (mem && WriteProcessMemory(p, mem, dll.c_str(), bytes, nullptr)) {
        auto ll = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE t = CreateRemoteThread(p, nullptr, 0, ll, mem, 0, nullptr);
        if (t) {
            WaitForSingleObject(t, 8000);
            CloseHandle(t);
            if (hasModule(pid, L"cpoldcrit.dll")) rc = 0;
        }
    }
    if (mem) VirtualFreeEx(p, mem, 0, MEM_RELEASE);
    CloseHandle(p);
    return rc;
}
