#include "rename_dialog.h"
#include "device_aliases.h"
#include "i18n.h"

#include <algorithm>
#include <vector>

namespace {
constexpr wchar_t kWindowClass[] = L"WirelessBatteryTextEditor";

struct EditorState {
    const wchar_t* title;
    const wchar_t* label;
    const wchar_t* invalidMessage;
    bool (*valid)(const std::wstring&);
    std::wstring initial;
    bool numeric = false;
    std::wstring result;
    HWND edit = nullptr;
    bool accepted = false;
};

EditorState* State(HWND hwnd) {
    return reinterpret_cast<EditorState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void SetControlFont(HWND control) {
    if (control) SendMessageW(control, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
}

LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
        return TRUE;
    }
    EditorState* state = State(hwnd);
    if (!state) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_CREATE: {
            HINSTANCE inst = GetModuleHandleW(nullptr);
            const int dpi = GetDpiForWindow(hwnd);
            auto scale = [dpi](int v) { return MulDiv(v, dpi, 96); };
            HWND label = CreateWindowExW(0, L"STATIC", state->label,
                                         WS_CHILD | WS_VISIBLE, scale(16), scale(18), scale(382), scale(22),
                                         hwnd, nullptr, inst, nullptr);
            state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", state->initial.c_str(),
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | (state->numeric ? ES_NUMBER : 0),
                                          scale(16), scale(45), scale(382), scale(25),
                                          hwnd, (HMENU)(INT_PTR)100, inst, nullptr);
            HWND save = CreateWindowExW(0, L"BUTTON", T(L"Save"),
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                        scale(220), scale(85), scale(85), scale(29),
                                        hwnd, (HMENU)(INT_PTR)IDOK, inst, nullptr);
            HWND cancel = CreateWindowExW(0, L"BUTTON", T(L"Cancel"),
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                          scale(313), scale(85), scale(85), scale(29),
                                          hwnd, (HMENU)(INT_PTR)IDCANCEL, inst, nullptr);
            SetControlFont(label);
            SetControlFont(state->edit);
            SetControlFont(save);
            SetControlFont(cancel);
            SendMessageW(state->edit, EM_SETLIMITTEXT, aliases::kMaxNameLength, 0);
            SetFocus(state->edit);
            SendMessageW(state->edit, EM_SETSEL, 0, -1);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK) {
                const int length = GetWindowTextLengthW(state->edit);
                std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1, 0);
                GetWindowTextW(state->edit, buffer.data(), (int)buffer.size());
                std::wstring value = aliases::Trim(buffer.data());
                if (!state->valid(value)) {
                    MessageBoxW(hwnd, state->invalidMessage, state->title, MB_OK | MB_ICONINFORMATION);
                    SetFocus(state->edit);
                    return 0;
                }
                state->result = std::move(value);
                state->accepted = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wp) == IDCANCEL) { DestroyWindow(hwnd); return 0; }
            break;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
bool ValidBatteryThreshold(const std::wstring& value) {
    return !value.empty() && value.size() <= 3 &&
           std::all_of(value.begin(), value.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }) &&
           std::stoi(value) <= 100;
}

bool PromptEdit(HWND owner, EditorState& state) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = inst;
    wc.lpfnWndProc = EditorProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    const int dpi = owner ? GetDpiForWindow(owner) : 96;
    const int width = MulDiv(420, dpi, 96), height = MulDiv(157, dpi, 96);
    POINT p{};
    GetCursorPos(&p);
    HMONITOR monitor = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    RECT area{};
    if (GetMonitorInfoW(monitor, &mi)) area = mi.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    int x = area.left + ((area.right - area.left - width) / 2);
    int y = area.top + ((area.bottom - area.top - height) / 2);
    x = std::max(x, static_cast<int>(area.left));
    y = std::max(y, static_cast<int>(area.top));

    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW, kWindowClass,
                                   state.title, WS_CAPTION | WS_SYSMENU | WS_POPUP,
                                   x, y, width, height, owner, nullptr, inst, &state);
    if (!dialog) return false;

    const bool ownerEnabled = owner && IsWindowEnabled(owner);
    if (ownerEnabled) EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    SetForegroundWindow(dialog);
    if (state.edit) {
        SetFocus(state.edit);
        SendMessageW(state.edit, EM_SETSEL, 0, -1);
    }
    MSG msg{};
    while (IsWindow(dialog)) {
        BOOL code = GetMessageW(&msg, nullptr, 0, 0);
        if (code <= 0) {
            if (code == 0) PostQuitMessage((int)msg.wParam);
            break;
        }
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (IsWindow(dialog)) DestroyWindow(dialog);
    if (ownerEnabled) EnableWindow(owner, TRUE);
    return state.accepted;
}
}  // namespace

bool PromptDeviceName(HWND owner, const std::wstring& currentName, std::wstring& result) {
    EditorState state{ T(L"Rename device"), T(L"Device display name:"), T(L"Enter a name (1-64 characters)."),
                       aliases::ValidName, currentName };
    if (!PromptEdit(owner, state)) return false;
    result = std::move(state.result);
    return true;
}

bool PromptBatteryThreshold(HWND owner, int& threshold) {
    const wchar_t* instruction = T(L"Enter a whole percentage from 0 to 100.");
    EditorState state{ T(L"Low-battery color threshold"), instruction, instruction,
                       ValidBatteryThreshold, std::to_wstring(threshold), true };
    if (!PromptEdit(owner, state)) return false;
    threshold = std::stoi(state.result);
    return true;
}
