#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <jsc/jsc.h>
#include <libsoup/soup.h>
#include <glib-unix.h>
#include "animbridge/desktop_bridge.hpp"
#include "animbridge/instance_lock.hpp"
#include "animbridge/ui_resources.hpp"
#include "animbridge/ui_message.hpp"
#include <algorithm>
#include <csignal>
#include <iostream>

using namespace animbridge;
using Json = nlohmann::json;
namespace {
constexpr const char* Origin = "animbridge://ui/";
constexpr const char* Document = "animbridge://ui/index.html";
GtkApplication* application;
GtkWidget* window;
WebKitWebView* browser;
std::unique_ptr<DesktopBridge> bridge;
std::unique_ptr<InstanceLock> ownership;
WebViewSession transport;
RuntimeContext context;
fs::path dataOverride;
std::atomic_bool closing{false};
bool automation = false, terminating = false, startupFailed = false;
std::vector<fs::path> pendingDocuments;
void dispatch(std::function<void()> task) {
    auto pending = new std::function<void()>(std::move(task));
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean { if (!closing) try { (*static_cast<std::function<void()>*>(data))(); } catch (...) {} return G_SOURCE_REMOVE; }, pending, [](gpointer data) { delete static_cast<std::function<void()>*>(data); });
}
void post(const Json& message) {
    auto script = "window.__animbridgeReceive?.(" + message.dump() + ")";
    webkit_web_view_evaluate_javascript(browser, script.c_str(), static_cast<gssize>(script.size()), nullptr, Document, nullptr, nullptr, nullptr);
}
void deliver(const Json& value) {
    if (closing) return;
    auto envelope = transport.envelope(value); if (envelope.is_null()) return;
    dispatch([value, envelope] {
        auto state = value.value("event", "") == "state" ? value.value("data", Json::object()) : value.value("result", Json::object());
        if (state.contains("settings")) webkit_web_view_set_zoom_level(browser, std::clamp(state["settings"].value("uiScale", 100) / 100.0, .75, 2.0));
        post(envelope);
    });
}
void nativeError(const std::string& message) {
    GtkWidget* alert = gtk_message_dialog_new(window ? GTK_WINDOW(window) : nullptr, GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", message.c_str());
    g_signal_connect(alert, "response", G_CALLBACK(+[](GtkDialog* dialog, gint, gpointer) { gtk_widget_destroy(GTK_WIDGET(dialog)); if (startupFailed) g_application_quit(G_APPLICATION(application)); }), nullptr); gtk_widget_show(alert);
}
gboolean finishClose(gpointer) {
    if (bridge && bridge->working()) return G_SOURCE_CONTINUE;
    closing = true; transport.invalidate(); bridge.reset(); if (window) gtk_widget_destroy(window); window = nullptr; g_application_quit(G_APPLICATION(application)); return G_SOURCE_REMOVE;
}
gboolean requestClose(GtkWidget*, GdkEvent*, gpointer) {
    if (bridge && bridge->busy() && !terminating) { deliver({{"event", "error"}, {"data", messageData(UiError("native.closeBusy", "Request cancellation and wait for the job to finish before closing."))}}); return TRUE; }
    if (bridge && bridge->working()) { bridge->cancel(); g_timeout_add(50, finishClose, nullptr); return TRUE; }
    finishClose(nullptr); return TRUE;
}
std::string label(const Json& payload, const char* key, const char* fallback) {
    auto text = payload.value(key, std::string(fallback));
    if (text.size() > 800 || !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr) || std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; })) throw std::runtime_error("Invalid native dialog label");
    return text;
}
struct PickRequest { DesktopPlatformServices::PickCompletion done; };
struct SaveRequest { DesktopPlatformServices::SaveCompletion done; };
DesktopPlatformServices services() {
    DesktopPlatformServices result;
    result.pick = [](const Json& payload, bool folder, DesktopPlatformServices::PickCompletion done) {
        dispatch([payload, folder, done] {
            try {
            auto title = label(payload, "title", folder ? "Select a folder" : "Open files");
            auto dialog = gtk_file_chooser_native_new(title.c_str(), GTK_WINDOW(window), folder ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER : GTK_FILE_CHOOSER_ACTION_OPEN, nullptr, nullptr);
            gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), payload.value("multiple", !folder));
            auto pending = new PickRequest{done};
            g_signal_connect_data(dialog, "response", G_CALLBACK(+[](GtkNativeDialog* native, gint response, gpointer data) {
                auto request = static_cast<PickRequest*>(data);
                try {
                    std::vector<fs::path> paths;
                    if (response == GTK_RESPONSE_ACCEPT) {
                        auto selected = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(native));
                        for (auto item = selected; item; item = item->next) { auto text = static_cast<char*>(item->data); if (!g_utf8_validate(text, -1, nullptr)) { g_slist_free_full(selected, g_free); throw std::runtime_error("The selected filename is not valid UTF-8"); } paths.push_back(fromUtf8(text)); }
                        g_slist_free_full(selected, g_free);
                    }
                    request->done(std::move(paths), {});
                } catch (const std::exception& e) { request->done({}, e.what()); }
                gtk_native_dialog_destroy(native); g_object_unref(native);
            }), pending, [](gpointer data, GClosure*) { delete static_cast<PickRequest*>(data); }, static_cast<GConnectFlags>(0));
            gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
            } catch (const std::exception& e) { done({}, e.what()); }
        });
    };
    result.save = [](const Json& payload, DesktopPlatformServices::SaveCompletion done) {
        dispatch([payload, done] {
            try {
            auto title = label(payload, "title", "Save job report");
            auto dialog = gtk_file_chooser_native_new(title.c_str(), GTK_WINDOW(window), GTK_FILE_CHOOSER_ACTION_SAVE, nullptr, nullptr);
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "animbridge-report.json"); gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
            auto pending = new SaveRequest{done};
            g_signal_connect_data(dialog, "response", G_CALLBACK(+[](GtkNativeDialog* native, gint response, gpointer data) {
                auto request = static_cast<SaveRequest*>(data);
                try { fs::path path; if (response == GTK_RESPONSE_ACCEPT) { auto text = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(native)); if (text) { if (!g_utf8_validate(text, -1, nullptr)) { g_free(text); throw std::runtime_error("The save filename is not valid UTF-8"); } path = fromUtf8(text); g_free(text); } } request->done(path, {}); }
                catch (const std::exception& e) { request->done({}, e.what()); }
                gtk_native_dialog_destroy(native); g_object_unref(native);
            }), pending, [](gpointer data, GClosure*) { delete static_cast<SaveRequest*>(data); }, static_cast<GConnectFlags>(0));
            gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
            } catch (const std::exception& e) { done({}, e.what()); }
        });
    };
    result.copyText = [](const std::string& text) { gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text.c_str(), static_cast<gint>(text.size())); gtk_clipboard_store(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD)); };
    result.openExternal = [](const std::string& url) { GError* error = nullptr; if (!g_app_info_launch_default_for_uri(url.c_str(), nullptr, &error)) { std::string message = error ? error->message : "Could not open browser"; g_clear_error(&error); throw std::runtime_error(message); } };
    result.reveal = [](const fs::path& path) {
        GError* error = nullptr; auto uri = g_filename_to_uri(path.c_str(), nullptr, &error);
        if (!uri) { g_clear_error(&error); throw std::runtime_error("Could not encode the selected path"); }
        auto bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error); GVariant* response = nullptr;
        if (bus) { const char* items[] = {uri, nullptr}; response = g_dbus_connection_call_sync(bus, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1", "ShowItems", g_variant_new("(^ass)", items, ""), nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error); g_object_unref(bus); }
        g_free(uri); if (response) { g_variant_unref(response); g_clear_error(&error); return Json{{"opened", true}, {"revealed", true}}; }
        g_clear_error(&error); auto parent = fs::is_directory(path) ? path : path.parent_path(); auto parentUri = g_filename_to_uri(parent.c_str(), nullptr, &error);
        if (!parentUri) { g_clear_error(&error); throw std::runtime_error("Could not open the parent folder"); }
        auto opened = g_app_info_launch_default_for_uri(parentUri, nullptr, &error); g_free(parentUri);
        if (!opened) { std::string message = error ? error->message : "No file manager is available"; g_clear_error(&error); throw std::runtime_error(message); }
        return Json{{"opened", true}, {"revealed", false}, {"fallback", "parentFolder"}};
    };
    return result;
}
void resourceRequest(WebKitURISchemeRequest* request, gpointer) {
    try {
        auto resource = readUiResource(runtimePaths().ui, webkit_uri_scheme_request_get_uri(request), Origin);
        if (!resource) { auto error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Static resource is not in the UI manifest"); webkit_uri_scheme_request_finish_error(request, error); g_error_free(error); return; }
        auto bytes = g_bytes_new(resource->bytes.data(), resource->bytes.size()); auto stream = g_memory_input_stream_new_from_bytes(bytes); g_bytes_unref(bytes);
        auto response = webkit_uri_scheme_response_new(stream, static_cast<gint64>(resource->bytes.size())); g_object_unref(stream);
        webkit_uri_scheme_response_set_status(response, 200, "OK"); webkit_uri_scheme_response_set_content_type(response, resource->mime.c_str());
        auto headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE); soup_message_headers_append(headers, "Content-Security-Policy", UiContentSecurityPolicy); soup_message_headers_append(headers, "X-Content-Type-Options", "nosniff"); soup_message_headers_append(headers, "Cache-Control", "no-store");
        webkit_uri_scheme_response_set_http_headers(response, headers); soup_message_headers_unref(headers);
        webkit_uri_scheme_request_finish_with_response(request, response); g_object_unref(response);
    } catch (...) { auto error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "Unable to read UI resource"); webkit_uri_scheme_request_finish_error(request, error); g_error_free(error); }
}
void receive(WebKitUserContentManager*, WebKitJavascriptResult* result, gpointer) {
    if (closing || !bridge || !transport.ready() || !webkit_web_view_get_uri(browser) || std::string(webkit_web_view_get_uri(browser)) != Document) return;
    auto value = webkit_javascript_result_get_js_value(result); auto encoded = jsc_value_to_json(value, 0); if (!encoded) return;
    try {
        auto request = Json::parse(encoded); g_free(encoded); encoded = nullptr;
        // The GTK message signal has no trustworthy frame URL. The host instead
        // enforces CSP frame-src/child-src none on every static document, allows
        // only the fixed document lifecycle, and issues only a top-view token.
        if (!transport.accept(request, Document, true, true)) return;
        if (request.value("action", "") == "window") { deliver({{"id", request.at("id")}, {"ok", true}, {"result", {{"nativeDragRegions", true}, {"customFrame", false}}}}); return; }
        bridge->handle(request);
    } catch (const std::exception& e) { deliver({{"event", "error"}, {"data", messageData(e)}}); }
    g_free(encoded);
}
gboolean decidePolicy(WebKitWebView*, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer) {
    bool allowed = false;
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        auto action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
        auto request = webkit_navigation_action_get_request(action);
        allowed = std::string(webkit_uri_request_get_uri(request)) == Document;
    } else if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        auto response = WEBKIT_RESPONSE_POLICY_DECISION(decision); auto request = webkit_response_policy_decision_get_request(response);
        allowed = readUiResource(runtimePaths().ui, webkit_uri_request_get_uri(request), Origin).has_value() && webkit_response_policy_decision_is_mime_type_supported(response);
    }
    if (allowed) webkit_policy_decision_use(decision); else webkit_policy_decision_ignore(decision); return TRUE;
}
void loadChanged(WebKitWebView* view, WebKitLoadEvent event, gpointer) {
    if (event == WEBKIT_LOAD_STARTED) transport.invalidate();
    if (event != WEBKIT_LOAD_FINISHED || closing || !webkit_web_view_get_uri(view) || std::string(webkit_web_view_get_uri(view)) != Document) return;
    try {
        post(transport.commit(Document, "linux", false));
        if (!pendingDocuments.empty()) { auto paths = std::move(pendingDocuments); pendingDocuments.clear(); bridge->ingest(paths, GrantSource::NativeDocument); }
    } catch (const std::exception& e) { nativeError(e.what()); }
}
void nativeDrop(GtkWidget*, GdkDragContext* drag, gint, gint, GtkSelectionData* data, guint, guint time, gpointer) {
    bool accepted = false;
    try {
        auto uris = gtk_selection_data_get_uris(data); std::vector<fs::path> paths;
        if (uris) { for (auto i = 0; uris[i]; ++i) { gchar* host = nullptr; GError* error = nullptr; auto filename = g_filename_from_uri(uris[i], &host, &error); if (filename && (!host || !*host || std::string(host) == "localhost") && g_utf8_validate(filename, -1, nullptr)) paths.push_back(fromUtf8(filename)); g_free(filename); g_free(host); g_clear_error(&error); } g_strfreev(uris); }
        if (!paths.empty() && bridge) { bridge->ingest(paths); accepted = true; }
    } catch (const std::exception& e) { deliver({{"event", "error"}, {"data", messageData(e)}}); }
    gtk_drag_finish(drag, accepted, FALSE, time);
}
void activate(GApplication*, gpointer) {
    if (window) { gtk_window_present(GTK_WINDOW(window)); return; }
    try {
        ownership = std::make_unique<InstanceLock>(context.dataRoot); if (!ownership->acquired()) throw std::runtime_error(ownership->diagnostic());
        window = gtk_application_window_new(application);
#if defined(ANIMBRIDGE_ENABLE_UI_TEST_HOOKS) && ANIMBRIDGE_ENABLE_UI_TEST_HOOKS
        gtk_window_set_title(GTK_WINDOW(window), "AnimBridge · G2 Harness");
#else
        gtk_window_set_title(GTK_WINDOW(window), "AnimBridge");
#endif
        gtk_window_set_default_size(GTK_WINDOW(window), 1480, 900); gtk_widget_set_size_request(window, 900, 640);
        auto data = webkit_website_data_manager_new("base-data-directory", utf8(context.dataRoot / "webkit").c_str(), "base-cache-directory", utf8(context.cacheRoot / "webkit").c_str(), nullptr);
        auto webContext = webkit_web_context_new_with_website_data_manager(data); g_object_unref(data);
        webkit_web_context_set_sandbox_enabled(webContext, TRUE);
        if (!webkit_web_context_get_sandbox_enabled(webContext)) throw std::runtime_error("WebKit sandbox is unavailable");
        webkit_web_context_set_automation_allowed(webContext, FALSE);
        auto security = webkit_web_context_get_security_manager(webContext);
        webkit_security_manager_register_uri_scheme_as_local(security, "animbridge"); webkit_security_manager_register_uri_scheme_as_secure(security, "animbridge"); webkit_security_manager_register_uri_scheme_as_cors_enabled(security, "animbridge");
        webkit_web_context_register_uri_scheme(webContext, "animbridge", resourceRequest, nullptr, nullptr);
        auto manager = webkit_user_content_manager_new(); g_signal_connect(manager, "script-message-received::animbridge", G_CALLBACK(receive), nullptr); webkit_user_content_manager_register_script_message_handler(manager, "animbridge");
        browser = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", webContext, "user-content-manager", manager, nullptr)); g_object_unref(manager); g_object_unref(webContext);
        auto settings = webkit_web_view_get_settings(browser); webkit_settings_set_enable_javascript(settings, TRUE); webkit_settings_set_javascript_can_open_windows_automatically(settings, FALSE); webkit_settings_set_allow_file_access_from_file_urls(settings, FALSE); webkit_settings_set_allow_universal_access_from_file_urls(settings, FALSE); webkit_settings_set_enable_media_stream(settings, FALSE);
#if defined(ANIMBRIDGE_ENABLE_UI_TEST_HOOKS) && ANIMBRIDGE_ENABLE_UI_TEST_HOOKS
        webkit_settings_set_enable_developer_extras(settings, automation);
#else
        webkit_settings_set_enable_developer_extras(settings, FALSE);
#endif
        g_signal_connect(browser, "decide-policy", G_CALLBACK(decidePolicy), nullptr); g_signal_connect(browser, "load-changed", G_CALLBACK(loadChanged), nullptr);
        g_signal_connect(browser, "load-failed", G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent, const gchar*, GError*, gpointer) -> gboolean { transport.invalidate(); return FALSE; }), nullptr);
        g_signal_connect(browser, "web-process-terminated", G_CALLBACK(+[](WebKitWebView*, WebKitWebProcessTerminationReason, gpointer) { transport.invalidate(); }), nullptr);
        g_signal_connect(browser, "create", G_CALLBACK(+[](WebKitWebView*, WebKitNavigationAction*, gpointer) -> WebKitWebView* { return nullptr; }), nullptr);
        g_signal_connect(browser, "permission-request", G_CALLBACK(+[](WebKitWebView*, WebKitPermissionRequest* request, gpointer) -> gboolean { webkit_permission_request_deny(request); return TRUE; }), nullptr);
        g_signal_connect(browser, "run-file-chooser", G_CALLBACK(+[](WebKitWebView*, WebKitFileChooserRequest* request, gpointer) -> gboolean { webkit_file_chooser_request_cancel(request); return TRUE; }), nullptr);
        g_signal_connect(browser, "context-menu", G_CALLBACK(+[](WebKitWebView*, WebKitContextMenu*, GdkEvent*, WebKitHitTestResult*, gpointer) -> gboolean { return TRUE; }), nullptr);
        auto activeContext = webkit_web_view_get_context(browser); g_signal_connect(activeContext, "download-started", G_CALLBACK(+[](WebKitWebContext*, WebKitDownload* download, gpointer) { webkit_download_cancel(download); }), nullptr);
        GtkTargetEntry targets[] = {{const_cast<gchar*>("text/uri-list"), 0, 0}}; gtk_drag_dest_set(GTK_WIDGET(browser), GTK_DEST_DEFAULT_ALL, targets, 1, GDK_ACTION_COPY); g_signal_connect(browser, "drag-data-received", G_CALLBACK(nativeDrop), nullptr);
        g_signal_connect(window, "delete-event", G_CALLBACK(requestClose), nullptr); gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(browser));
        bridge = std::make_unique<DesktopBridge>(deliver, services(), dataOverride); gtk_widget_show_all(window); webkit_web_view_load_uri(browser, Document);
        g_unix_signal_add(SIGTERM, [](gpointer) -> gboolean { terminating = true; if (bridge) bridge->cancel(); requestClose(nullptr, nullptr, nullptr); return G_SOURCE_REMOVE; }, nullptr);
    } catch (const std::exception& e) { startupFailed = true; std::cerr << e.what() << '\n'; nativeError(e.what()); }
}
void openFiles(GApplication* app, GFile** files, gint count, const gchar*, gpointer) {
    std::vector<fs::path> paths;
    for (gint i = 0; i < count; ++i) { auto path = g_file_get_path(files[i]); if (path && g_utf8_validate(path, -1, nullptr)) paths.push_back(fromUtf8(path)); g_free(path); }
    activate(app, nullptr);
    if (bridge && transport.ready()) bridge->ingest(paths, GrantSource::NativeDocument); else pendingDocuments.insert(pendingDocuments.end(), paths.begin(), paths.end());
}
}
int main(int argc, char** argv) {
    try {
        clearBrowserEnvironmentOverrides(); std::vector<std::string> args{argv[0]};
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--data-dir" && i + 1 < argc) dataOverride = fs::absolute(fromUtf8(argv[++i]));
#if defined(ANIMBRIDGE_ENABLE_UI_TEST_HOOKS) && ANIMBRIDGE_ENABLE_UI_TEST_HOOKS
            else if (arg == "--ui-test" || arg == "--automation") automation = true;
#endif
            else if (arg.starts_with("--")) throw std::runtime_error("Unknown AnimBridge option");
            else args.push_back(utf8(fs::absolute(fromUtf8(arg))));
        }
        context = resolveRuntimeContext(dataOverride); fs::create_directories(context.dataRoot);
        auto appId = "org.animbridge.desktop.r" + textSha256(utf8(fs::canonical(context.dataRoot))).substr(0, 24);
        application = gtk_application_new(appId.c_str(), G_APPLICATION_HANDLES_OPEN);
        g_signal_connect(application, "activate", G_CALLBACK(activate), nullptr); g_signal_connect(application, "open", G_CALLBACK(openFiles), nullptr);
        std::vector<char*> nativeArgs; for (auto& arg : args) nativeArgs.push_back(arg.data()); nativeArgs.push_back(nullptr);
        auto status = g_application_run(G_APPLICATION(application), static_cast<int>(args.size()), nativeArgs.data());
        closing = true; transport.invalidate(); bridge.reset(); ownership.reset(); g_object_unref(application); return startupFailed ? 1 : status;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
