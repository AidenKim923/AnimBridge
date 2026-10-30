#include "animbridge/desktop_bridge.hpp"
#include "animbridge/app_config.hpp"
#include "animbridge/ui_message.hpp"
#include "animbridge/ui_resources.hpp"
#include "animbridge/instance_lock.hpp"
#include "resource.h"
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <winhttp.h>
#include <shobjidl.h>
#include <sddl.h>
#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

using namespace animbridge;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT BridgeMessage = WM_APP + 1;
constexpr UINT WindowCommandMessage = WM_APP + 2;
constexpr UINT UiDispatchMessage = WM_APP + 3;
constexpr UINT NativeDocumentMessage = WM_APP + 4;
WebViewSession transport;
std::atomic_bool closing{false};
constexpr wchar_t Origin[] = L"https://animbridge.local/";
HWND window = nullptr;
ComPtr<ICoreWebView2Environment> environment;
ComPtr<ICoreWebView2Controller> controller;
ComPtr<ICoreWebView2> webview;
std::unique_ptr<DesktopBridge> bridge;
bool automation = false;
bool closeAfterRead = false;
struct MoveGesture {
    bool active = false;
    int id = 0;
    POINT origin{};
    double x = 0, y = 0, scale = 1;
} moveGesture;
struct ResizeGesture {
    bool active = false;
    int id = 0;
    std::string direction;
    RECT bounds{};
    double x = 0, y = 0, scale = 1;
    int minimumWidth = 0, minimumHeight = 0;
} resizeGesture;
constexpr UINT_PTR ReadCloseTimer = 42;
std::vector<fs::path> initialPaths;
fs::path dataRootOverride, appDataRoot;
std::wstring documentPipe;
std::thread documentReceiver;
HANDLE documentStop = nullptr;
// Reads the user SID (as a string) from an open process token.
bool tokenUserSid(HANDLE token, std::string& sid) {
    DWORD length = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &length);  // The first call only reports the size.
    std::vector<unsigned char> bytes(length);
    if (!GetTokenInformation(token, TokenUser, bytes.data(), length, &length)) return false;
    PWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &text)) return false;
    sid = utf8(fs::path(text));
    LocalFree(text);
    return true;
}
std::string currentUserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw std::runtime_error("Could not inspect native IPC owner");
    std::string sid;
    const bool ok = tokenUserSid(token, sid);
    CloseHandle(token);
    if (!ok) throw std::runtime_error("Could not inspect native IPC owner");
    return sid;
}
// True when the process belongs to the same Windows user as this one.
bool sameUser(DWORD pid) {
    auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    HANDLE token = nullptr;
    const bool opened = OpenProcessToken(process, TOKEN_QUERY, &token);
    CloseHandle(process);
    if (!opened) return false;
    std::string sid;
    const bool ok = tokenUserSid(token, sid);
    CloseHandle(token);
    return ok && sid == currentUserSid();
}
void forwardDocuments() {
    if (initialPaths.empty() || !WaitNamedPipeW(documentPipe.c_str(), 2000)) return;
    auto pipe = CreateFileW(documentPipe.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return;
    DWORD pid = 0;
    if (GetNamedPipeServerProcessId(pipe, &pid) && sameUser(pid)) {
        nlohmann::json paths = nlohmann::json::array();
        for (const auto& path : initialPaths) paths.push_back(utf8(fs::absolute(path)));
        auto bytes = nlohmann::json{{"protocolVersion", 1}, {"paths", paths}}.dump();
        DWORD written = 0;
        if (bytes.size() <= 1024 * 1024) WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    }
    CloseHandle(pipe);
}
// Parses one document-IPC request and hands its paths to the UI thread.
// Malformed, oversized or relative-path requests are dropped silently.
void postDocumentRequest(const char* data, size_t size) {
    try {
        auto request = nlohmann::json::parse(data, data + size);
        const bool valid = request.value("protocolVersion", 0) == 1 && request.contains("paths")
            && request["paths"].is_array() && request["paths"].size() <= 256;
        if (!valid) return;
        auto paths = std::make_unique<std::vector<fs::path>>();
        for (const auto& item : request["paths"]) {
            auto path = fromUtf8(item.get<std::string>());
            if (!path.is_absolute()) throw std::runtime_error("Document IPC requires absolute paths");
            paths->push_back(path);
        }
        // The UI thread owns the vector once the message is posted.
        if (PostMessageW(window, NativeDocumentMessage, 0, reinterpret_cast<LPARAM>(paths.get()))) paths.release();
    } catch (...) {}
}
void startDocumentReceiver() {
    documentStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!documentStop) throw std::runtime_error("Could not create native document IPC");
    const auto owner = currentUserSid();
    documentReceiver = std::thread([owner] {
        try {
            // Only the current user may connect to the pipe.
            auto descriptorText = fromUtf8("D:P(A;;GA;;;" + owner + ")").wstring();
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptorText.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return;
            SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
            while (WaitForSingleObject(documentStop, 0) != WAIT_OBJECT_0) {
                // One pipe instance at a time; FIRST_PIPE_INSTANCE refuses to share the name with another process.
                auto pipe = CreateNamedPipeW(documentPipe.c_str(),
                    PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                    1, 0, 1024 * 1024, 2000, &security);
                if (pipe == INVALID_HANDLE_VALUE) break;
                OVERLAPPED operation{};
                operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                auto connected = ConnectNamedPipe(pipe, &operation);
                auto code = connected ? ERROR_SUCCESS : GetLastError();
                // Index 0 means shutdown was requested; index 1 means the pending I/O finished.
                HANDLE events[] = {documentStop, operation.hEvent};
                bool ready = code == ERROR_PIPE_CONNECTED || connected;
                if (code == ERROR_IO_PENDING) ready = WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1;
                DWORD pid = 0;
                if (ready && GetNamedPipeClientProcessId(pipe, &pid) && sameUser(pid)) {
                    std::vector<char> bytes(1024 * 1024);
                    DWORD count = 0;
                    ResetEvent(operation.hEvent);
                    auto read = ReadFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &count, &operation);
                    if (!read && GetLastError() == ERROR_IO_PENDING && WaitForMultipleObjects(2, events, FALSE, 2000) == WAIT_OBJECT_0 + 1) {
                        read = GetOverlappedResult(pipe, &operation, &count, FALSE);
                    }
                    if (read && count) postDocumentRequest(bytes.data(), count);
                }
                CancelIoEx(pipe, nullptr);
                DWORD ignored = 0;
                GetOverlappedResult(pipe, &operation, &ignored, TRUE);
                CloseHandle(operation.hEvent);
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
            }
            LocalFree(descriptor);
        } catch (...) { /* A failed document channel must never create another writer. */ }
    });
}
void stopDocumentReceiver() {
    if (documentStop) SetEvent(documentStop);
    if (documentReceiver.joinable()) documentReceiver.join();
    if (documentStop) CloseHandle(documentStop);
    documentStop = nullptr;
}
struct MutexLifetime {
    HANDLE handle = nullptr;
    ~MutexLifetime() { if (handle) CloseHandle(handle); }
};
std::wstring instanceKey() {
    auto data = fs::weakly_canonical(appDataRoot).make_preferred().wstring();
    // Windows paths are case-insensitive; equivalent spellings share state ownership.
    if (!data.empty()) CharLowerBuffW(data.data(), static_cast<DWORD>(data.size()));
    return fromUtf8(textSha256(utf8(fs::path(data)))).wstring();
}
void activateExisting(const std::wstring& className) {
    HWND existing = nullptr;
    // The first instance may still be creating its window when a double click launches another.
    for (unsigned attempt = 0; attempt < 40 && !existing; ++attempt) {
        existing = FindWindowW(className.c_str(), nullptr);
        if (!existing) Sleep(50);
    }
    if (!existing) return;
    ShowWindow(existing, IsIconic(existing) ? SW_RESTORE : SW_SHOW);
    auto popup = GetLastActivePopup(existing);
    SetForegroundWindow(popup && IsWindowVisible(popup) ? popup : existing);
}
bool localUri(const wchar_t* uri) { return uri && std::wstring_view(uri).starts_with(Origin); }
void send(const nlohmann::json& value) {
    if (closing) return;
    auto outgoing = transport.envelope(value);
    if (outgoing.is_null()) return;
    auto text = std::make_unique<std::wstring>(fromUtf8(outgoing.dump()).wstring());
    // The UI thread owns the string once the message is posted.
    if (PostMessageW(window, BridgeMessage, 0, reinterpret_cast<LPARAM>(text.get()))) text.release();
}
int resizeBorder(HWND hwnd) { return std::max(1, MulDiv(6, GetDpiForWindow(hwnd), USER_DEFAULT_SCREEN_DPI)); }
LRESULT resizeHitTest(HWND hwnd, LPARAM position) {
    if (IsZoomed(hwnd)) return HTCLIENT;
    RECT bounds{};
    GetWindowRect(hwnd, &bounds);
    const POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    const int border = resizeBorder(hwnd);
    const bool left = point.x < bounds.left + border, right = point.x >= bounds.right - border;
    const bool top = point.y < bounds.top + border, bottom = point.y >= bounds.bottom - border;
    if (top) return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
    if (bottom) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
    return left ? HTLEFT : right ? HTRIGHT : HTCLIENT;
}
UINT resizeCommand(std::string_view direction) {
    constexpr std::pair<std::string_view, UINT> edges[] = {
        {"left", WMSZ_LEFT}, {"right", WMSZ_RIGHT}, {"top", WMSZ_TOP}, {"bottom", WMSZ_BOTTOM},
        {"top-left", WMSZ_TOPLEFT}, {"top-right", WMSZ_TOPRIGHT},
        {"bottom-left", WMSZ_BOTTOMLEFT}, {"bottom-right", WMSZ_BOTTOMRIGHT}
    };
    for (const auto& [name, edge] : edges) if (direction == name) return SC_SIZE | edge;
    return 0;
}
bool updateResizeGesture(const nlohmann::json& payload) {
    const auto phase = payload.value("phase", "");
    const int id = payload.value("gesture", 0);
    if (phase == "begin") {
        const auto direction = payload.value("direction", "");
        if (!resizeCommand(direction) || id <= 0) return false;
        resizeGesture = {};
        if (IsZoomed(window) || IsIconic(window)) return true;
        resizeGesture.x = payload.value("x", 0.0);
        resizeGesture.y = payload.value("y", 0.0);
        if (!std::isfinite(resizeGesture.x) || !std::isfinite(resizeGesture.y)) return false;
        resizeGesture.active = true;
        resizeGesture.id = id;
        resizeGesture.direction = direction;
        GetWindowRect(window, &resizeGesture.bounds);
        const auto dpi = GetDpiForWindow(window);
        resizeGesture.scale = dpi / static_cast<double>(USER_DEFAULT_SCREEN_DPI);
        resizeGesture.minimumWidth = MulDiv(900, dpi, USER_DEFAULT_SCREEN_DPI);
        resizeGesture.minimumHeight = MulDiv(640, dpi, USER_DEFAULT_SCREEN_DPI);
        return true;
    }
    if (phase != "update" && phase != "end" && phase != "cancel") return false;
    if (!resizeGesture.active || resizeGesture.id != id) return true;
    if (IsZoomed(window) || IsIconic(window)) { resizeGesture.active = false; return true; }
    auto bounds = resizeGesture.bounds;
    if (phase != "cancel") {
        const double x = payload.value("x", resizeGesture.x), y = payload.value("y", resizeGesture.y);
        if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1e7 || std::abs(y) > 1e7) return false;
        const LONG dx = std::lround((x - resizeGesture.x) * resizeGesture.scale);
        const LONG dy = std::lround((y - resizeGesture.y) * resizeGesture.scale);
        const auto& direction = resizeGesture.direction;
        if (direction.find("left") != std::string::npos) bounds.left = std::min(bounds.right - resizeGesture.minimumWidth, bounds.left + dx);
        if (direction.find("right") != std::string::npos) bounds.right = std::max(bounds.left + resizeGesture.minimumWidth, bounds.right + dx);
        if (direction.find("top") != std::string::npos) bounds.top = std::min(bounds.bottom - resizeGesture.minimumHeight, bounds.top + dy);
        if (direction.find("bottom") != std::string::npos) bounds.bottom = std::max(bounds.top + resizeGesture.minimumHeight, bounds.bottom + dy);
    }
    // Absolute deltas also work when WebView delivers the whole gesture after release.
    SetWindowPos(window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
    if (phase != "update") resizeGesture.active = false;
    return true;
}
// Header drags use the page's screen coordinates, like resize. WebView2 non-client
// regions and SC_MOVE failed in v0.2.4, and sampling the cursor at processing time
// fails because messages can arrive after release.
bool updateMoveGesture(const nlohmann::json& payload) {
    const auto phase = payload.value("phase", "");
    const int id = payload.value("gesture", 0);
    if (phase == "begin") {
        moveGesture = {};
        if (id <= 0) return false;
        if (IsZoomed(window) || IsIconic(window)) return true;
        const double x = payload.value("x", 0.0), y = payload.value("y", 0.0);
        if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1e7 || std::abs(y) > 1e7) return false;
        RECT bounds{};
        if (!GetWindowRect(window, &bounds)) return true;
        moveGesture.x = x;
        moveGesture.y = y;
        moveGesture.origin = {bounds.left, bounds.top};
        moveGesture.scale = GetDpiForWindow(window) / static_cast<double>(USER_DEFAULT_SCREEN_DPI);
        moveGesture.id = id;
        moveGesture.active = true;
        return true;
    }
    if (phase != "update" && phase != "end" && phase != "cancel") return false;
    if (!moveGesture.active || moveGesture.id != id) return true;
    if (IsZoomed(window) || IsIconic(window)) { moveGesture.active = false; return true; }
    POINT target = moveGesture.origin;
    if (phase != "cancel") {
        const double x = payload.value("x", moveGesture.x), y = payload.value("y", moveGesture.y);
        if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1e7 || std::abs(y) > 1e7) return false;
        target.x += std::lround((x - moveGesture.x) * moveGesture.scale);
        target.y += std::lround((y - moveGesture.y) * moveGesture.scale);
    }
    SetWindowPos(window, nullptr, target.x, target.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (phase != "update") moveGesture.active = false;
    return true;
}
bool monitorInfo(HWND hwnd, MONITORINFO& monitor) {
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
    // Leave the trigger strip of an auto-hide taskbar exposed on its own monitor.
    // A custom client frame otherwise covers it when rcWork equals rcMonitor.
    for (UINT edge : {ABE_LEFT, ABE_TOP, ABE_RIGHT, ABE_BOTTOM}) {
        APPBARDATA bar{sizeof(bar)}; bar.uEdge = edge; bar.rc = monitor.rcMonitor;
        if (!SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &bar)) continue;
        constexpr LONG trigger = 2;
        if (edge == ABE_LEFT && monitor.rcWork.left == monitor.rcMonitor.left) monitor.rcWork.left += trigger;
        if (edge == ABE_TOP && monitor.rcWork.top == monitor.rcMonitor.top) monitor.rcWork.top += trigger;
        if (edge == ABE_RIGHT && monitor.rcWork.right == monitor.rcMonitor.right) monitor.rcWork.right -= trigger;
        if (edge == ABE_BOTTOM && monitor.rcWork.bottom == monitor.rcMonitor.bottom) monitor.rcWork.bottom -= trigger;
    }
    return true;
}
void resize() {
    if (!controller) return;
    // A minimized window has a 0x0 client area. Shrinking WebView2 to 0x0 and back can
    // leave a black surface after restore (e.g. via Alt+Tab), so hide it instead and keep
    // the last real size.
    if (IsIconic(window)) { controller->put_IsVisible(FALSE); return; }
    RECT bounds{}; GetClientRect(window, &bounds);
    controller->put_Bounds(bounds);
    controller->put_IsVisible(TRUE);
    controller->NotifyParentWindowPositionChanged();
}
nlohmann::json windowState() {
    double zoom = 1;
    if (controller) controller->get_ZoomFactor(&zoom);
    return {{"maximized", IsZoomed(window) != FALSE}, {"active", GetForegroundWindow() == window},
        {"resizeBorderCss", 6.0 / zoom}};
}
void publishWindowState() { send({{"event", "windowState"}, {"data", windowState()}}); }
bool handleWindowCommand(const nlohmann::json& request) {
    if (request.value("action", "") != "window") return false;
    const auto payload = request.value("payload", nlohmann::json::object());
    const auto command = payload.value("command", "");
    const auto id = request.value("id", nlohmann::json());
    UINT systemCommand = 0;
    if (command == "minimize") systemCommand = SC_MINIMIZE;
    else if (command == "maximize") systemCommand = IsZoomed(window) ? SC_RESTORE : SC_MAXIMIZE;
    else if (command == "close") systemCommand = SC_CLOSE;
    else if (command == "resize") {
        if (!updateResizeGesture(payload)) {
            send({{"id", id}, {"ok", false}, {"error", messageData(UiError("native.windowResizeUnsupported", "Unsupported window resize direction."))}});
            return true;
        }
    }
    else if (command == "move") {
        if (!updateMoveGesture(payload)) {
            send({{"id", id}, {"ok", false}, {"error", messageData(UiError("native.windowMoveUnsupported", "Unsupported window move request."))}});
            return true;
        }
    }
    else if (command != "getState") {
        send({{"id", id}, {"ok", false}, {"error", messageData(UiError("native.windowCommandUnsupported", "Unsupported window command."))}});
        return true;
    }
    send({{"id", id}, {"ok", true}, {"result", windowState()}});
    // Run after returning from WebMessageReceived: moving/resizing enters a modal
    // Windows message loop, which must not block a WebView callback.
    if (systemCommand) {
        POINT cursor{};
        GetCursorPos(&cursor);
        PostMessageW(window, WindowCommandMessage, systemCommand, MAKELPARAM(cursor.x, cursor.y));
    }
    return true;
}
void fatal(const std::wstring& description, HRESULT code = S_OK) {
    auto message = description;
    if (FAILED(code)) {
        wchar_t suffix[40]{};
        swprintf_s(suffix, L"\nHRESULT: 0x%08X", static_cast<unsigned>(code));
        message += suffix;
    }
    MessageBoxW(window, message.c_str(), L"AnimBridge startup error", MB_OK | MB_ICONERROR);
}
std::wstring dialogLabel(const nlohmann::json& payload, const char* key, const wchar_t* fallback) {
    if (!payload.contains(key)) return fallback;
    if (!payload[key].is_string()) throw UiError("native.dialogLabelInvalid", "Dialog labels must be plain text of at most 200 characters.");
    auto text = fromUtf8(payload[key].get<std::string>()).wstring();
    if (text.size() > 200 || std::any_of(text.begin(), text.end(), [](wchar_t c) { return c < 32 || c == 127; }))
        throw UiError("native.dialogLabelInvalid", "Dialog labels must be plain text of at most 200 characters.");
    return text.empty() ? fallback : text;
}
std::vector<fs::path> pick(const nlohmann::json& payload, bool folder) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        throw UiError("native.pickerUnavailable", "Could not open the file picker.");
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    DWORD mode = FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (folder) mode |= FOS_PICKFOLDERS;
    else mode |= FOS_FILEMUSTEXIST | (payload.value("multiple", true) ? FOS_ALLOWMULTISELECT : 0);
    dialog->SetOptions(options | mode);
    const auto title = dialogLabel(payload, "title", folder ? L"Select a folder" : L"Open a package or animation file");
    dialog->SetTitle(title.c_str());
    if (!folder) {
        const auto filterLabel = dialogLabel(payload, "filterLabel", L"AnimBridge supported files"), allFilesLabel = dialogLabel(payload, "allFilesLabel", L"All files");
        const auto filter = payload.value("filter", "all");
        const wchar_t* pattern = filter == "exe" ? L"*.exe" : filter == "fbx" ? L"*.fbx" : filter == "uproject" ? L"*.uproject" : L"*.unitypackage;*.anim;*.fbx;*.json;*.uasset";
        const COMDLG_FILTERSPEC filters[] = {{filterLabel.c_str(), pattern}, {allFilesLabel.c_str(), L"*.*"}};
        dialog->SetFileTypes(2, filters);
    }
    // A starting folder only positions the dialog; the user's pick is still what gets approved.
    if (payload.contains("initialFolder") && payload["initialFolder"].is_string()) {
        ComPtr<IShellItem> start;
        const auto folderPath = fromUtf8(payload["initialFolder"].get<std::string>());
        if (fs::is_directory(folderPath) && SUCCEEDED(SHCreateItemFromParsingName(folderPath.c_str(), nullptr, IID_PPV_ARGS(&start)))) dialog->SetFolder(start.Get());
    }
    auto result = dialog->Show(window);
    std::vector<fs::path> paths;
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return paths;
    if (FAILED(result)) throw UiError("native.pickFailed", "File selection failed.");
    ComPtr<IShellItemArray> items;
    if (FAILED(dialog->GetResults(&items))) return paths;
    DWORD count = 0;
    items->GetCount(&count);
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        if (FAILED(items->GetItemAt(i, &item))) continue;
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            paths.emplace_back(path);
            CoTaskMemFree(path);
        }
    }
    return paths;
}
fs::path savePicker(const nlohmann::json& payload) {
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        throw UiError("native.savePickerUnavailable", "Could not open the report save dialog.");
    }
    const auto title = dialogLabel(payload, "title", L"Save job report");
    const auto filterLabel = dialogLabel(payload, "filterLabel", L"JSON report");
    dialog->SetTitle(title.c_str());
    dialog->SetFileName(L"animationbridge-report.json");
    dialog->SetDefaultExtension(L"json");
    const COMDLG_FILTERSPEC filters[] = {{filterLabel.c_str(), L"*.json"}};
    dialog->SetFileTypes(1, filters);
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
    if (FAILED(dialog->Show(window))) return {};
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return {};
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
    fs::path result(path);
    CoTaskMemFree(path);
    return result;
}
void dispatch(std::function<void()> callback) {
    auto owned = std::make_unique<std::function<void()>>(std::move(callback));
    if (!closing && PostMessageW(window, UiDispatchMessage, 0, reinterpret_cast<LPARAM>(owned.get()))) owned.release();
}
// HTTPS-only GET for the release update check: bounded time, bounded body, no redirects to other schemes.
std::string httpsGet(const std::string& url) {
    struct Handle {
        HINTERNET value = nullptr;
        ~Handle() { if (value) WinHttpCloseHandle(value); }
    };
    constexpr size_t MaxBody = 2u * 1024u * 1024u;
    const auto wideUrl = fromUtf8(url).wstring();
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS || !parts.lpszHostName) throw std::runtime_error("Update check requires an HTTPS URL");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) + std::wstring(parts.lpszExtraInfo ? parts.lpszExtraInfo : L"", parts.lpszExtraInfo ? parts.dwExtraInfoLength : 0);
    const auto agent = fromUtf8(std::string("AnimBridge-UpdateCheck/") + std::string(animbridge::app::Version)).wstring();
    Handle session, connection, request;
    session.value = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.value) throw std::runtime_error("Could not start the network session");
    WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /* TLS 1.3 where available */;
    WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    connection.value = WinHttpConnect(session.value, host.c_str(), parts.nPort, 0);
    if (!connection.value) throw std::runtime_error("Could not connect to the update server");
    request.value = WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request.value) throw std::runtime_error("Could not create the update request");
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
    static const wchar_t headers[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28";
    if (!WinHttpSendRequest(request.value, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.value, nullptr)) throw std::runtime_error("The update server did not respond");
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200) throw std::runtime_error("The update server returned HTTP " + std::to_string(status));
    std::string body;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.value, &available)) throw std::runtime_error("Could not read the update response");
        if (available == 0) break;
        if (body.size() + available > MaxBody) throw std::runtime_error("The update response is too large");
        const auto offset = body.size(); body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.value, body.data() + offset, available, &read)) throw std::runtime_error("Could not read the update response");
        body.resize(offset + read);
        if (read == 0) break;
    }
    return body;
}
DesktopPlatformServices nativeServices() {
    DesktopPlatformServices services;
    services.httpGet = httpsGet;
    services.unityHubEditorRoot = defaultUnityHubEditorRoot;
    // The pickers run on the UI thread (COM dialogs need its message loop); results are
    // reported back through the completion callback.
    services.pick = [](const nlohmann::json& payload, bool folder, DesktopPlatformServices::PickCompletion done) {
        dispatch([payload, folder, done] {
            try {
                done(pick(payload, folder), {});
            } catch (const std::exception& e) {
                done({}, e.what());
            }
        });
    };
    services.save = [](const nlohmann::json& payload, DesktopPlatformServices::SaveCompletion done) {
        dispatch([payload, done] {
            try {
                done(savePicker(payload), {});
            } catch (const std::exception& e) {
                done({}, e.what());
            }
        });
    };
    services.openExternal = [](const std::string& url) {
        auto wide = fromUtf8(url).wstring();
        // ShellExecute reports success with a value greater than 32.
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
            throw std::runtime_error("Could not open the default browser");
        }
    };
    services.reveal = [](const fs::path& path) {
        // A folder is opened directly; a file is shown selected inside Explorer.
        auto folder = fs::is_directory(path);
        auto args = folder ? std::wstring() : L"/select,\"" + path.wstring() + L"\"";
        const wchar_t* target = folder ? path.c_str() : L"explorer.exe";
        const wchar_t* parameters = folder ? nullptr : args.c_str();
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", target, parameters, nullptr, SW_SHOWNORMAL)) <= 32) {
            throw std::runtime_error("Could not reveal the selected path");
        }
        return nlohmann::json{{"opened", true}, {"revealed", !folder}};
    };
    services.copyText = [](const std::string& value) {
        auto text = fromUtf8(value).wstring();
        if (!OpenClipboard(window)) throw std::runtime_error("Could not open clipboard");
        auto bytes = (text.size() + 1) * sizeof(wchar_t);
        auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!memory) {
            CloseClipboard();
            throw std::runtime_error("Could not allocate clipboard");
        }
        auto destination = GlobalLock(memory);
        if (!destination) {
            GlobalFree(memory);
            CloseClipboard();
            throw std::runtime_error("Could not lock clipboard");
        }
        memcpy(destination, text.c_str(), bytes);
        GlobalUnlock(memory);
        EmptyClipboard();
        // On success the clipboard owns the memory; on failure it is still ours to free.
        if (!SetClipboardData(CF_UNICODETEXT, memory)) {
            GlobalFree(memory);
            CloseClipboard();
            throw std::runtime_error("Could not set clipboard");
        }
        CloseClipboard();
    };
    return services;
}
// Collects the file-system paths of files dropped on the page. WebView2 exposes them
// only as AdditionalObjects of the message, never inside the JSON payload.
std::vector<fs::path> droppedPaths(ICoreWebView2WebMessageReceivedEventArgs* args) {
    std::vector<fs::path> dropped;
    ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> extended;
    if (FAILED(args->QueryInterface(IID_PPV_ARGS(&extended)))) return dropped;
    ComPtr<ICoreWebView2ObjectCollectionView> objects;
    if (FAILED(extended->get_AdditionalObjects(&objects)) || !objects) return dropped;
    UINT32 count = 0;
    objects->get_Count(&count);
    for (UINT32 i = 0; i < count; ++i) {
        ComPtr<IUnknown> object;
        if (FAILED(objects->GetValueAtIndex(i, &object)) || !object) continue;
        ComPtr<ICoreWebView2File> file;
        if (FAILED(object.As(&file))) continue;
        PWSTR path = nullptr;
        if (SUCCEEDED(file->get_Path(&path))) {
            if (path && *path) dropped.emplace_back(path);
            CoTaskMemFree(path);
        }
    }
    return dropped;
}
// Handles one message posted by the page. Only the bundled index.html may talk to the host.
void handleWebMessage(ICoreWebView2WebMessageReceivedEventArgs* args) {
    PWSTR source = nullptr;
    args->get_Source(&source);
    const bool permitted = source && std::wstring_view(source) == L"https://animbridge.local/index.html";
    CoTaskMemFree(source);
    if (!permitted || !bridge) return;
    PWSTR data = nullptr;
    if (FAILED(args->get_WebMessageAsJson(&data))) return;
    try {
        auto request = nlohmann::json::parse(utf8(fs::path(data)));
        if (request.is_string()) request = nlohmann::json::parse(request.get<std::string>());
        if (!transport.accept(request, "https://animbridge.local/index.html", true, true)) {
            CoTaskMemFree(data);
            return;
        }
        if (handleWindowCommand(request)) {
            CoTaskMemFree(data);
            return;
        }
        std::vector<fs::path> dropped;
        if (request.value("action", "") == "drop") dropped = droppedPaths(args);
        bridge->handle(request, dropped);
    } catch (const std::exception& e) {
        send({{"event", "error"}, {"data", messageData(e)}});
    }
    CoTaskMemFree(data);
}
// Serves bundled UI files from memory with a strict CSP; everything else under the
// virtual host is refused with 403.
void serveUiResource(ICoreWebView2WebResourceRequestedEventArgs* args) {
    ComPtr<ICoreWebView2WebResourceRequest> request;
    args->get_Request(&request);
    PWSTR uri = nullptr;
    request->get_Uri(&uri);
    auto resource = uri ? readUiResource(runtimePaths().ui, utf8(fs::path(uri)), "https://animbridge.local/") : std::optional<UiResource>{};
    ComPtr<ICoreWebView2WebResourceResponse> response;
    if (resource) {
        ComPtr<IStream> stream;
        CreateStreamOnHGlobal(nullptr, TRUE, &stream);
        ULONG written = 0;
        if (stream) stream->Write(resource->bytes.data(), static_cast<ULONG>(resource->bytes.size()), &written);
        LARGE_INTEGER start{};
        if (stream) stream->Seek(start, STREAM_SEEK_SET, nullptr);
        auto headers = fromUtf8("Content-Type: " + resource->mime
            + "\r\nContent-Security-Policy: " + UiContentSecurityPolicy
            + "\r\nX-Content-Type-Options: nosniff\r\nCache-Control: no-store").wstring();
        environment->CreateWebResourceResponse(stream.Get(), 200, L"OK", headers.c_str(), &response);
    } else {
        environment->CreateWebResourceResponse(nullptr, 403, L"Local application only", L"Content-Type: text/plain", &response);
    }
    args->put_Response(response.Get());
    CoTaskMemFree(uri);
}
// Runs after each page load: writes the smoke-test report (automation only), sends the
// bridge bootstrap message and delivers files passed on the command line.
void onNavigationCompleted(ICoreWebView2NavigationCompletedEventArgs* args) {
    try {
        BOOL success = FALSE;
        args->get_IsSuccess(&success);
        if (automation) {
            PWSTR version = nullptr;
            environment->get_BrowserVersionString(&version);
            auto root = appDataRoot / "diagnostics";
            fs::create_directories(root);
            std::ofstream log(root / "host-smoke.json");
            log << nlohmann::json{{"navigationSucceeded", success != FALSE},
                {"runtime", version ? utf8(fs::path(version)) : ""},
                {"origin", "https://animbridge.local/index.html"}}.dump(2);
            CoTaskMemFree(version);
        }
        if (success) {
            auto bootstrap = fromUtf8(transport.commit("https://animbridge.local/index.html", "windows", true).dump()).wstring();
            webview->PostWebMessageAsJson(bootstrap.c_str());
        } else {
            transport.invalidate();
        }
        if (success && !initialPaths.empty()) {
            bridge->ingest(initialPaths, GrantSource::CommandLine);
            initialPaths.clear();
        }
        if (success) publishWindowState();
    } catch (const std::exception& e) {
        transport.invalidate();
        fatal(fromUtf8(e.what()).wstring());
    }
}
void configure() {
    ComPtr<ICoreWebView2Settings> settings;
    webview->get_Settings(&settings);
    settings->put_AreDefaultContextMenusEnabled(FALSE);
#if defined(ANIMBRIDGE_ENABLE_UI_TEST_HOOKS) && ANIMBRIDGE_ENABLE_UI_TEST_HOOKS
    settings->put_AreDevToolsEnabled(automation);
#else
    settings->put_AreDevToolsEnabled(FALSE);
#endif
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_IsZoomControlEnabled(FALSE);
    settings->put_AreHostObjectsAllowed(FALSE);
    settings->put_IsWebMessageEnabled(TRUE);
    ComPtr<ICoreWebView2Controller2> dark;
    if (SUCCEEDED(controller.As(&dark))) dark->put_DefaultBackgroundColor({255, 8, 8, 8});
    ComPtr<ICoreWebView2_3> mapping;
    if (FAILED(webview.As(&mapping))) { fatal(L"Update the WebView2 Runtime to the latest version."); return; }
    auto ui = runtimePaths().ui;
    if (!fs::is_regular_file(ui / "index.html")) { fatal(L"The installation is missing ui/index.html. Reinstall AnimBridge."); return; }
    auto hr = mapping->SetVirtualHostNameToFolderMapping(L"animbridge.local", ui.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
    if (FAILED(hr)) { fatal(L"Could not connect the local UI folder.", hr); return; }
    EventRegistrationToken token{};
    // Only the bundled index.html may be navigated to; a new page also starts a new bridge session.
    webview->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
        PWSTR uri = nullptr;
        args->get_Uri(&uri);
        if (!uri || std::wstring_view(uri) != L"https://animbridge.local/index.html") args->put_Cancel(TRUE);
        else transport.invalidate();
        CoTaskMemFree(uri);
        return S_OK;
    }).Get(), &token);
    webview->add_FrameNavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
        args->put_Cancel(TRUE); return S_OK;
    }).Get(), &token);
    webview->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>([](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
        args->put_Handled(TRUE); return S_OK;
    }).Get(), &token);
    webview->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>([](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
        args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY); return S_OK;
    }).Get(), &token);
    webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    webview->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>([](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
        serveUiResource(args);
        return S_OK;
    }).Get(), &token);
    // If the page's renderer dies (driver reset, out of memory), WebView2 shows a black or
    // empty surface until the page is reloaded. Reload it so the window recovers by itself.
    webview->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>([](ICoreWebView2* sender, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
        COREWEBVIEW2_PROCESS_FAILED_KIND kind{}; args->get_ProcessFailedKind(&kind);
        if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED || kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE) sender->Reload();
        return S_OK;
    }).Get(), &token);
    // Downloads are never allowed; the host has no download UI.
    ComPtr<ICoreWebView2_4> downloads;
    if (SUCCEEDED(webview.As(&downloads))) {
        downloads->add_DownloadStarting(Callback<ICoreWebView2DownloadStartingEventHandler>([](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
            args->put_Cancel(TRUE);
            return S_OK;
        }).Get(), &token);
    }
    webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>([](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
        handleWebMessage(args);
        return S_OK;
    }).Get(), &token);
    webview->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>([](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
        onNavigationCompleted(args);
        return S_OK;
    }).Get(), &token);
    resize();
    webview->Navigate(L"https://animbridge.local/index.html");
}
void initializeWebView() {
    auto userData = appDataRoot / "webview2";
    fs::create_directories(userData);
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
#if defined(ANIMBRIDGE_ENABLE_UI_TEST_HOOKS) && ANIMBRIDGE_ENABLE_UI_TEST_HOOKS
    options->put_AdditionalBrowserArguments(automation ? L"--remote-debugging-port=9223 --remote-debugging-address=127.0.0.1 --disable-background-networking --no-first-run" : L"--disable-background-networking --no-first-run");
#else
    options->put_AdditionalBrowserArguments(L"--disable-background-networking --no-first-run");
#endif
    const auto& browser = runtimePaths().webView2;
    auto hr = CreateCoreWebView2EnvironmentWithOptions(browser.empty() ? nullptr : browser.c_str(), userData.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result) || !env) { fatal(L"Could not start Microsoft Edge WebView2 Runtime. Check the installed runtime.", result); return S_OK; }
            environment = env;
            env->CreateCoreWebView2Controller(window, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([](HRESULT code, ICoreWebView2Controller* created) -> HRESULT {
                if (FAILED(code) || !created) { fatal(L"Could not create the UI controller.", code); return S_OK; }
                controller = created;
                controller->get_CoreWebView2(&webview);
                controller->put_IsVisible(TRUE);
                configure();
                return S_OK;
            }).Get());
            return S_OK;
        }).Get());
    if (FAILED(hr)) fatal(L"WebView2 Runtime was not found. Microsoft Edge WebView2 Evergreen Runtime is required.", hr);
}
LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_NCCALCSIZE:
        // Preserve the normal resizable window styles and system behavior, while
        // replacing the caption and frame with our dark client area.
        if (wp) {
            auto& client = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0];
            if (IsZoomed(hwnd)) {
                MONITORINFO monitor{sizeof(monitor)};
                if (monitorInfo(hwnd, monitor)) client = monitor.rcWork;
            }
        }
        return 0;
    case WM_NCHITTEST:
        return resizeHitTest(hwnd, lp);
    case WM_CREATE:
        window = hwnd;
        DragAcceptFiles(hwnd, TRUE);
        {
            const BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
            const COLORREF border = RGB(35, 35, 35);
            DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
        }
        try {
            bridge = std::make_unique<DesktopBridge>(send, nativeServices(), dataRootOverride);
            startDocumentReceiver();
            initializeWebView();
            return 0;
        } catch (const std::exception& e) {
            stopDocumentReceiver();
            fatal(fromUtf8(e.what()).wstring());
            return -1;
        }
    case WM_SIZE: resize(); publishWindowState(); return 0;
    case WM_MOVE: if (controller) controller->NotifyParentWindowPositionChanged(); break;
    case WM_ACTIVATE:
        // Coming back via Alt+Tab: make sure the WebView is shown and knows where it is.
        if (LOWORD(wp) != WA_INACTIVE) resize();
        publishWindowState(); break;
    case WM_DPICHANGED: {
        // Windows suggests a new rectangle that keeps the same physical size on the new monitor.
        auto suggested = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        resize();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto limits = reinterpret_cast<MINMAXINFO*>(lp);
        const auto dpi = GetDpiForWindow(hwnd);
        limits->ptMinTrackSize = {MulDiv(900, dpi, USER_DEFAULT_SCREEN_DPI), MulDiv(640, dpi, USER_DEFAULT_SCREEN_DPI)};
        MONITORINFO monitor{sizeof(monitor)};
        if (monitorInfo(hwnd, monitor)) {
            limits->ptMaxPosition = {monitor.rcWork.left - monitor.rcMonitor.left, monitor.rcWork.top - monitor.rcMonitor.top};
            limits->ptMaxSize = {monitor.rcWork.right - monitor.rcWork.left, monitor.rcWork.bottom - monitor.rcWork.top};
            limits->ptMinTrackSize.x = std::min(limits->ptMinTrackSize.x, limits->ptMaxSize.x);
            limits->ptMinTrackSize.y = std::min(limits->ptMinTrackSize.y, limits->ptMaxSize.y);
        }
        return 0;
    }
    case UiDispatchMessage: {
        std::unique_ptr<std::function<void()>> action(reinterpret_cast<std::function<void()>*>(lp));
        if (!closing) (*action)();
        return 0;
    }
    case NativeDocumentMessage: {
        std::unique_ptr<std::vector<fs::path>> paths(reinterpret_cast<std::vector<fs::path>*>(lp));
        if (bridge && !closing) {
            // Before the page finished loading there is no bridge session; keep the files for later.
            if (transport.ready()) bridge->ingest(*paths, GrantSource::NativeDocument);
            else initialPaths.insert(initialPaths.end(), paths->begin(), paths->end());
        }
        return 0;
    }
    case WindowCommandMessage:
        if ((wp & 0xfff0) == SC_SIZE) ReleaseCapture();
        SendMessageW(hwnd, WM_SYSCOMMAND, wp, lp);
        return 0;
    case WM_DROPFILES: {
        auto drop = reinterpret_cast<HDROP>(wp);
        std::vector<fs::path> paths;
        auto count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            auto length = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(length + 1, L'\0');
            DragQueryFileW(drop, i, path.data(), length + 1);
            path.resize(length);
            paths.emplace_back(path);
        }
        DragFinish(drop);
        bridge->ingest(paths);
        return 0;
    }
    case BridgeMessage: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
        if (controller) try {
            auto value = nlohmann::json::parse(utf8(fs::path(*text)));
            auto state = value.value("event", "") == "state" ? value.value("data", nlohmann::json::object()) : value.value("result", nlohmann::json::object());
            if (state.contains("settings") && state["settings"].contains("uiScale") && state["settings"]["uiScale"].is_number()) {
                double zoom = std::clamp(state["settings"]["uiScale"].get<double>() / 100.0, .75, 2.0), previous = 1.0;
                controller->get_ZoomFactor(&previous);
                if (std::abs(zoom - previous) > .0001) {
                    controller->put_ZoomFactor(zoom);
                    publishWindowState();
                }
            }
        } catch (...) { /* A failed response must not block delivery to the UI. */ }
        if (webview) webview->PostWebMessageAsJson(text->c_str());
        return 0;
    }
    case WM_CLOSE:
        if (bridge && bridge->busy()) {
            send({{"event", "error"}, {"data", messageData(UiError("native.closeBusy", "A job is running. Request cancellation and wait for it to stop safely before closing."))}});
            return 0;
        }
        if (bridge && bridge->working()) {
            // Wait (polling) for cancellation to finish before really closing.
            bridge->cancel();
            closeAfterRead = true;
            SetTimer(hwnd, ReadCloseTimer, 50, nullptr);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_QUERYENDSESSION:
        if (bridge) bridge->cancel();
        return TRUE;
    case WM_ENDSESSION:
        if (wp) {
            if (bridge) bridge->cancel();
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_TIMER:
        if (wp == ReadCloseTimer && closeAfterRead && bridge && !bridge->working()) {
            KillTimer(hwnd, ReadCloseTimer);
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_DESTROY:
        closing = true;
        transport.invalidate();
        stopDocumentReceiver();
        bridge.reset();
        if (controller) controller->Close();
        webview.Reset();
        controller.Reset();
        environment.Reset();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    clearBrowserEnvironmentOverrides();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int count = 0; auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    try {
        for (int i = 1; i < count; ++i) {
            auto argument = std::wstring_view(args[i]);
#if defined(ANIMBRIDGE_ENABLE_UI_TEST_HOOKS) && ANIMBRIDGE_ENABLE_UI_TEST_HOOKS
            if (argument == L"--ui-test" || argument == L"--automation") automation = true;
            else
#endif
            if (argument == L"--data-dir") {
                if (++i >= count || !*args[i]) throw std::runtime_error("--data-dir requires a directory");
                dataRootOverride = fs::absolute(args[i]);
            } else if (argument.starts_with(L"--")) throw std::runtime_error("Unknown AnimBridge option");
            else initialPaths.emplace_back(args[i]);
        }
        appDataRoot = dataRootOverride.empty() ? defaultDataRoot() : dataRootOverride;
    } catch (const std::exception& error) {
        if (args) LocalFree(args);
        fatal(fromUtf8(error.what()).wstring());
        return 4;
    }
    if (args) LocalFree(args);
    std::wstring key;
    try {
        key = instanceKey();
    } catch (...) {
        MessageBoxW(nullptr, L"Could not determine the app data location.", L"AnimBridge startup error", MB_OK | MB_ICONERROR);
        return 4;
    }
    auto className = L"AnimBridgeWebViewWindow." + key;
    documentPipe = L"\\\\.\\pipe\\AnimBridge.Documents." + key;
    InstanceLock ownership(appDataRoot);
    if (!ownership.acquired()) { forwardDocuments(); activateExisting(className); return 0; }
    auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 2;
    WNDCLASSEXW klass{sizeof(klass)}; klass.lpfnWndProc = proc; klass.hInstance = instance;
    klass.lpszClassName = className.c_str(); klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hbrBackground = CreateSolidBrush(RGB(8, 8, 8)); klass.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_ANIMBRIDGE), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    klass.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_ANIMBRIDGE), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    RegisterClassExW(&klass);
    auto hwnd = CreateWindowExW(0, klass.lpszClassName, L"AnimBridge", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1480, 940, nullptr, nullptr, instance, nullptr);
    if (!hwnd) { CoUninitialize(); return 3; }
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ShowWindow(hwnd, show); UpdateWindow(hwnd);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    DeleteObject(klass.hbrBackground);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
