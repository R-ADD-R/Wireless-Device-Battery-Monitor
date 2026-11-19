// Misiom Alpha (3206:7003, 2.4 GHz), using the Windows audio driver's control interface.
// The hardware match and read-only battery command come from the vendor's O HUB 1.0.10
// package (https://www.misiom.cn/download/, shengmang-misiom.inf and the application's battery query).
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

// This vendor IOCTL uses METHOD_NEITHER. Keep its buffers and handles alive if the
// driver has not completed cancellation by the poll deadline. Only the polling
// thread uses this request; the OS cleans up any pending operation at process exit.
struct Transfer {
    HANDLE file = INVALID_HANDLE_VALUE;
    OVERLAPPED operation{};
    std::array<DWORD, 32> request{ 0x17, 0x43, 0x17, kGetBattery, 0, 0, 1 };
    std::array<BYTE, 128> reply{};
};
Transfer g_transfer;

void CloseTransfer() {
    CloseHandle(g_transfer.operation.hEvent);
    CloseHandle(g_transfer.file);
    g_transfer.operation = {};
    g_transfer.file = INVALID_HANDLE_VALUE;
}

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

int QueryBattery(const wchar_t* path, ULONGLONG deadline) {
    auto& t = g_transfer;
    t.file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (t.file == INVALID_HANDLE_VALUE) return -1;
    t.operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!t.operation.hEvent) {
        CloseHandle(t.file);
        t.file = INVALID_HANDLE_VALUE;
        return -1;
    }

    // Word 6 selects an input request. Reset the output before every query.
    t.reply.fill(0);
    DWORD bytes = 0;
    bool received = DeviceIoControl(t.file, kUsbControl, t.request.data(), sizeof(t.request),
                                    t.reply.data(), sizeof(t.reply), &bytes, &t.operation);
    if (!received && GetLastError() == ERROR_IO_PENDING) {
        const auto now = GetTickCount64();
        const DWORD remaining = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
        if (WaitForSingleObject(t.operation.hEvent, remaining) != WAIT_OBJECT_0) {
            CancelIoEx(t.file, &t.operation);
            return -1;
        }
        received = GetOverlappedResult(t.file, &t.operation, &bytes, FALSE);
        if (!received && GetLastError() == ERROR_IO_INCOMPLETE) {
            CancelIoEx(t.file, &t.operation);
            return -1;
        }
    }
    CloseTransfer();

    // O HUB treats zero as telemetry not ready. Require the command echo and a
    // nonzero percentage before publishing.
    return received && t.reply[0] == kGetBattery && t.reply[1] > 0 && t.reply[1] <= 100 ? t.reply[1] : -1;
}

int PollInterfaces(GUID& kind, std::wstring_view suffix, ULONGLONG deadline) {
    auto paths = PresentInterfaces(kind);
    for (const wchar_t* p = paths.data(); p && *p && GetTickCount64() < deadline;) {
        std::wstring_view path(p);
        if (path.size() >= kAlphaInterface.size() + suffix.size() &&
            EqualIgnoringCase(path.substr(0, kAlphaInterface.size()), kAlphaInterface) &&
            EqualIgnoringCase(path.substr(path.size() - suffix.size()), suffix)) {
            int percent = QueryBattery(p, deadline);
            if (percent >= 0 || g_transfer.file != INVALID_HANDLE_VALUE) return percent;
        }
        p += path.size() + 1;
    }
    return -1;
}

}  // namespace

void PollMisiomAlpha(PollContext& ctx) {
    if (g_transfer.file != INVALID_HANDLE_VALUE) {
        // Discard a late response. Until it finishes, leave its buffers in place
        // and avoid issuing another request to this driver.
        DWORD discarded = 0;
        if (!GetOverlappedResult(g_transfer.file, &g_transfer.operation, &discarded, FALSE) &&
            GetLastError() == ERROR_IO_INCOMPLETE) return;
        CloseTransfer();
    }
    if (ctx.IsClaimed(kVid, kPid)) return;
    const ULONGLONG deadline = GetTickCount64() + kTimeoutMs;
    int percent = PollInterfaces(kAudioClass, LR"(\xmosctl)", deadline);
    if (percent < 0 && g_transfer.file == INVALID_HANDLE_VALUE && GetTickCount64() < deadline)
        percent = PollInterfaces(kControlClass, LR"(\control)", deadline);
    if (percent < 0) return;

    DeviceStatus status;
    status.key = L"misiom:3206:7003";
    status.name = L"Misiom Alpha";
    status.via = L"2.4 GHz receiver";
    status.kind = DeviceKind::Headset;
    status.percent = percent;
    ctx.Add(status, kVid, kPid);
}
