#include "../src/drivers/drivers.h"
#include <cfgmgr32.h>
#include <cassert>
#include <cstring>
#include <cwchar>

static constexpr wchar_t kWrongPath[] = LR"(\\?\USB#VID_3206&PID_7004&MI_00#test#{guid}\control)";
static constexpr wchar_t kWrongInterface[] = LR"(\\?\USB#VID_3206&PID_7003&MI_01#test#{guid}\control)";
static constexpr wchar_t kWrongSuffix[] = LR"(\\?\USB#VID_3206&PID_7003&MI_00#test#{guid}\audio)";
static constexpr wchar_t kTargetPath[] = LR"(\\?\usb#vid_3206&pid_7003&mi_00#test#{guid}\CoNtRoL)";
static constexpr wchar_t kOldDriverPath[] = LR"(\\?\USB#VID_3206&PID_7003&MI_00#test#{guid}\xmosctl)";
static constexpr wchar_t kPresent[] =
    LR"(\\?\USB#VID_3206&PID_7004&MI_00#test#{guid}\control)" L"\0"
    LR"(\\?\USB#VID_3206&PID_7003&MI_01#test#{guid}\control)" L"\0"
    LR"(\\?\USB#VID_3206&PID_7003&MI_00#test#{guid}\audio)" L"\0"
    LR"(\\?\usb#vid_3206&pid_7003&mi_00#test#{guid}\CoNtRoL)" L"\0";
static constexpr wchar_t kLegacy[] =
    LR"(\\?\USB#VID_3206&PID_7003&MI_00#test#{guid}\xmosctl)" L"\0";
static const HANDLE kFile = reinterpret_cast<HANDLE>(13);
static const HANDLE kEvent = reinterpret_cast<HANDLE>(15);
static bool present = true, legacy = false, pending = false, timeout = false;
static DWORD error = ERROR_IO_PENDING;
static ULONGLONG tick = 10000;
static BYTE responseId = 0x87, responsePercent = 56;
static unsigned opens = 0, queries = 0, cancels = 0, closes = 0;

CONFIGRET MockListSize(ULONG* size, GUID* kind, const wchar_t*, ULONG) {
    if (!present) *size = 1;
    else if (kind->Data1 == 0x50894fa4) *size = legacy ? 1 : ARRAYSIZE(kPresent);
    else *size = legacy ? ARRAYSIZE(kLegacy) : 1;
    return CR_SUCCESS;
}
CONFIGRET MockList(GUID* kind, const wchar_t*, wchar_t* list, ULONG size, ULONG) {
    std::wmemcpy(list, kind->Data1 == 0x50894fa4 ? kPresent : kLegacy, size);
    return CR_SUCCESS;
}
HANDLE MockOpen(const wchar_t* p, DWORD access, DWORD share, void*, DWORD disposition, DWORD flags, HANDLE) {
    assert(std::wcscmp(p, kWrongPath) && std::wcscmp(p, kWrongInterface) && std::wcscmp(p, kWrongSuffix));
    assert(std::wcscmp(p, legacy ? kOldDriverPath : kTargetPath) == 0);
    assert(access == (GENERIC_READ | GENERIC_WRITE) && share == (FILE_SHARE_READ | FILE_SHARE_WRITE));
    assert(disposition == OPEN_EXISTING && flags == FILE_FLAG_OVERLAPPED);
    ++opens;
    return kFile;
}
HANDLE MockEvent(void*, BOOL, BOOL, const wchar_t*) { return kEvent; }
BOOL MockIo(HANDLE f, DWORD code, void* in, DWORD inSize, void* out, DWORD outSize, DWORD*, OVERLAPPED* o) {
    assert(f == kFile && o->hEvent == kEvent && code == 0x2214D3 && inSize == 128 && outSize == 128);
    DWORD expected[32] = {0x17, 0x43, 0x17, 0x87, 0, 0, 1};
    assert(std::memcmp(expected, in, inSize) == 0);
    auto* reply = static_cast<BYTE*>(out);
    assert(reply[0] == 0 && reply[1] == 0);
    reply[0] = responseId;
    reply[1] = responsePercent;
    ++queries;
    if (pending) error = ERROR_IO_PENDING;
    return !pending;
}
DWORD MockError() { return error; }
ULONGLONG MockTick() { return tick; }
DWORD MockWait(HANDLE e, DWORD ms) {
    assert(e == kEvent && ms == 1000);
    if (timeout) tick += ms;
    return timeout ? WAIT_TIMEOUT : WAIT_OBJECT_0;
}
BOOL MockResult(HANDLE f, OVERLAPPED* o, DWORD*, BOOL wait) {
    assert(f == kFile && o->hEvent == kEvent && !wait);
    if (!timeout) return TRUE;
    error = ERROR_IO_INCOMPLETE;
    return FALSE;
}
BOOL MockCancel(HANDLE f, OVERLAPPED* o) { assert(f == kFile && o->hEvent == kEvent); ++cancels; return TRUE; }
BOOL MockClose(HANDLE h) { assert(h == kFile || h == kEvent); ++closes; return TRUE; }

#define CM_Get_Device_Interface_List_SizeW MockListSize
#define CM_Get_Device_Interface_ListW MockList
#define CreateFileW MockOpen
#define CreateEventW MockEvent
#define DeviceIoControl MockIo
#define GetLastError MockError
#define GetTickCount64 MockTick
#define WaitForSingleObject MockWait
#define GetOverlappedResult MockResult
#define CancelIoEx MockCancel
#define CloseHandle MockClose
#include "../src/drivers/misiom.cpp"

int main() {
    PollContext c;
    c.Claim(0x3206, 0x7003);
    PollMisiomAlpha(c);
    assert(c.out.empty() && opens == 0);
    c.claimed.clear();
    present = false;
    PollMisiomAlpha(c);
    assert(c.out.empty() && opens == 0);
    present = true;
    PollMisiomAlpha(c);
    assert(c.out.size() == 1 && c.out[0].percent == 56 && c.out[0].kind == DeviceKind::Headset);
    assert(c.out[0].name == L"Misiom Alpha" && c.out[0].vid == 0x3206 && c.IsClaimed(0x3206, 0x7003));
    assert(opens == 1 && queries == 1 && closes == 2);
    for (BYTE bad : {BYTE(0), BYTE(101)}) {
        PollContext invalid;
        responsePercent = bad;
        PollMisiomAlpha(invalid);
        assert(invalid.out.empty());
    }
    responsePercent = 100;
    responseId = 0x88;
    PollContext invalid;
    PollMisiomAlpha(invalid);
    assert(invalid.out.empty());
    responseId = 0x87;
    pending = true;
    legacy = true;
    PollContext older;
    PollMisiomAlpha(older);
    assert(older.out.size() == 1 && older.out[0].percent == 100 && cancels == 0);
    legacy = false;
    timeout = true;
    PollContext delayed;
    PollMisiomAlpha(delayed);
    assert(delayed.out.empty() && cancels == 1 && closes == 2 * (opens - 1));

    unsigned previousOpens = opens;
    PollMisiomAlpha(delayed);
    assert(delayed.out.empty() && opens == previousOpens && closes == 2 * (opens - 1));

    timeout = false;
    responsePercent = 80;
    PollMisiomAlpha(delayed);
    assert(delayed.out.size() == 1 && delayed.out[0].percent == 80 && closes == 2 * opens);
    pending = false;
    present = false;
    assert(DumpMisiomAlpha().find(L"No present vendor audio control interface matched") != std::wstring::npos);
    present = true;
    legacy = true;
    responseId = 0;
    responsePercent = 0;
    const auto diagnostic = DumpMisiomAlpha();
    assert(diagnostic.find(L"\\xmosctl: Battery query completed, bytes returned: 0") != std::wstring::npos);
    assert(diagnostic.find(L"reply prefix: 00 00, no usable reading") != std::wstring::npos);
}
