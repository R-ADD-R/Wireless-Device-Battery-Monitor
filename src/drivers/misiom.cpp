// Misiom Alpha (3206:7003, 2.4 GHz), using the Windows audio driver's control interface.
// The hardware match and read-only battery command come from the vendor's O HUB 1.0.10
// package (shengmang-misiom.inf and the application's battery query).
#include "drivers.h"

#include <array>
#include <cfgmgr32.h>
#include <string_view>

namespace {

constexpr USHORT kVid = 0x3206;
constexpr USHORT kPid = 0x7003;
constexpr BYTE kGetBattery = 0x87;
constexpr DWORD kUsbControl = 0x2214D3;
constexpr DWORD kTimeoutMs = 1000;
constexpr std::wstring_view kAlphaInterface = LR"(\\?\USB#VID_3206&PID_7003&MI_00#)";

// Some versions expose xmosctl under the audio interface class. The bundled INF
// registers the dedicated control class instead.
GUID kAudioClass = { 0x6994ad04, 0x93ef, 0x11d0, { 0xa3, 0xcc, 0, 0xa0, 0xc9, 0x22, 0x31, 0x96 } };
GUID kControlClass = { 0x50894fa4, 0x9206, 0x4a91, { 0x87, 0x28, 0xcf, 0x6f, 0x26, 0x8f, 0x27, 0x1d } };

bool EqualIgnoringCase(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() &&
           CompareStringOrdinal(a.data(), (int)a.size(), b.data(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

std::vector<wchar_t> PresentInterfaces(GUID& kind) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        ULONG size = 0;
        if (CM_Get_Device_Interface_List_SizeW(&size, &kind, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
            size < 2) return {};
        std::vector<wchar_t> paths(size);
        CONFIGRET result = CM_Get_Device_Interface_ListW(&kind, nullptr, paths.data(), size, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (result == CR_SUCCESS) return paths;
        if (result != CR_BUFFER_SMALL) break;
    }
    return {};
}

int QueryBattery(const wchar_t* path) {
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (file == INVALID_HANDLE_VALUE) return -1;

    // The driver takes 32 little-endian words. Word 6 selects an input request.
    std::array<DWORD, 32> request{ 0x17, 0x43, 0x17, kGetBattery, 0, 0, 1 };
    std::array<BYTE, 128> reply{};
    OVERLAPPED transfer{};
    transfer.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool received = false;
    if (transfer.hEvent) {
        DWORD bytes = 0;
        received = DeviceIoControl(file, kUsbControl, request.data(), sizeof(request),
                                   reply.data(), sizeof(reply), &bytes, &transfer);
        if (!received && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(transfer.hEvent, kTimeoutMs) == WAIT_OBJECT_0) {
                received = GetOverlappedResult(file, &transfer, &bytes, FALSE);
            } else {
                CancelIoEx(file, &transfer);
                GetOverlappedResult(file, &transfer, &bytes, TRUE);  // keep both buffers alive through cancellation
            }
        }
        CloseHandle(transfer.hEvent);
    }
    CloseHandle(file);

    // O HUB treats a zero reading from this query as telemetry not ready. A reply
    // with the command echoed and a nonzero percentage is required before publishing.
    return received && reply[0] == kGetBattery && reply[1] > 0 && reply[1] <= 100 ? reply[1] : -1;
}

int PollInterfaces(GUID& kind, std::wstring_view suffix) {
    auto paths = PresentInterfaces(kind);
    for (const wchar_t* p = paths.data(); p && *p;) {
        std::wstring_view path(p);
        if (path.size() >= kAlphaInterface.size() + suffix.size() &&
            EqualIgnoringCase(path.substr(0, kAlphaInterface.size()), kAlphaInterface) &&
            EqualIgnoringCase(path.substr(path.size() - suffix.size()), suffix)) {
            int percent = QueryBattery(p);
            if (percent >= 0) return percent;
        }
        p += path.size() + 1;
    }
    return -1;
}

}  // namespace

void PollMisiomAlpha(PollContext& ctx) {
    if (ctx.IsClaimed(kVid, kPid)) return;
    int percent = PollInterfaces(kAudioClass, LR"(\xmosctl)");
    if (percent < 0) percent = PollInterfaces(kControlClass, LR"(\control)");
    if (percent < 0) return;

    DeviceStatus status;
    status.key = L"misiom:3206:7003";
    status.name = L"Misiom Alpha";
    status.via = L"2.4 GHz receiver";
    status.kind = DeviceKind::Headset;
    status.percent = percent;
    ctx.Add(status, kVid, kPid);
}
