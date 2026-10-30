#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include "animbridge/desktop_bridge.hpp"
#include "animbridge/instance_lock.hpp"
#include "animbridge/ui_resources.hpp"
#include "animbridge/ui_message.hpp"
#include <csignal>
#include <algorithm>

using namespace animbridge;
using Json = nlohmann::json;
namespace {
constexpr const char* Origin = "animbridge://ui/";
constexpr const char* Document = "animbridge://ui/index.html";
std::unique_ptr<DesktopBridge> bridge;
std::unique_ptr<InstanceLock> ownership;
WebViewSession transport;
RuntimeContext context;
fs::path dataOverride;
std::vector<fs::path> documents;
std::atomic_bool closing{false};
bool terminating = false;
bool automation = false;
NSWindow* mainWindow;
WKWebView* browser;
dispatch_source_t terminationSource;
NSString* ns(const std::string& text) { return [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding]; }
std::string string(NSString* text) { if (!text) return {}; const char* bytes = text.UTF8String; if (!bytes) throw std::runtime_error("Invalid UTF-8 in native path"); return bytes; }
void deliver(const Json& value) {
    if (closing) return;
    auto stamped = transport.envelope(value); if (stamped.is_null()) return;
    auto script = ns("window.__animbridgeReceive?.(" + stamped.dump() + ")");
    Json ownedValue = value;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (closing || !browser) return;
        auto state = ownedValue.value("event", "") == "state" ? ownedValue.value("data", Json::object()) : ownedValue.value("result", Json::object());
        if (state.contains("settings")) browser.pageZoom = std::clamp(state["settings"].value("uiScale", 100) / 100.0, .75, 2.0);
        [browser evaluateJavaScript:script completionHandler:nil];
    });
}
void failure(const std::exception& error) { NSAlert* alert = [[NSAlert alloc] init]; alert.messageText = @"AnimBridge"; alert.informativeText = ns(error.what()); [alert runModal]; }
bool stopWindow() {
    if (!bridge) return true;
    if (bridge->busy() && !terminating) { deliver({{"event", "error"}, {"data", messageData(UiError("native.closeBusy", "Request cancellation and wait for the job to finish before closing."))}}); return false; }
    if (bridge->working()) {
        bridge->cancel();
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ if (stopWindow()) [mainWindow close]; });
        return false;
    }
    closing = true; transport.invalidate(); bridge.reset(); return true;
}
DesktopPlatformServices services() {
    DesktopPlatformServices result;
    result.pick = [](const Json& payload, bool folder, DesktopPlatformServices::PickCompletion done) {
        Json ownedPayload = payload;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (closing) return;
            try {
            NSOpenPanel* panel = [NSOpenPanel openPanel]; panel.canChooseDirectories = folder; panel.canChooseFiles = !folder;
            panel.allowsMultipleSelection = ownedPayload.value("multiple", !folder); panel.resolvesAliases = YES;
            panel.title = ns(ownedPayload.value("title", folder ? "Select a folder" : "Open files"));
            // macOS Editors are selected as .app bundles, resolved to their native
            // executable by the grant registry after selection.
            panel.treatsFilePackagesAsDirectories = NO;
            [panel beginSheetModalForWindow:mainWindow completionHandler:^(NSModalResponse response) {
                if (closing) return;
                try { std::vector<fs::path> paths; if (response == NSModalResponseOK) for (NSURL* url in panel.URLs) if (url.isFileURL) paths.push_back(fromUtf8(string(url.path))); done(std::move(paths), {}); }
                catch (const std::exception& e) { done({}, e.what()); }
            }];
            } catch (const std::exception& e) { done({}, e.what()); }
        });
    };
    result.save = [](const Json& payload, DesktopPlatformServices::SaveCompletion done) {
        Json ownedPayload = payload;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (closing) return;
            try {
            NSSavePanel* panel = [NSSavePanel savePanel]; panel.title = ns(ownedPayload.value("title", "Save job report")); panel.nameFieldStringValue = @"animbridge-report.json"; panel.canCreateDirectories = YES;
            // NSSavePanel performs the native replacement confirmation.
            [panel beginSheetModalForWindow:mainWindow completionHandler:^(NSModalResponse response) {
                if (closing) return;
                try { done(response == NSModalResponseOK && panel.URL.isFileURL ? fromUtf8(string(panel.URL.path)) : fs::path{}, {}); }
                catch (const std::exception& e) { done({}, e.what()); }
            }];
            } catch (const std::exception& e) { done({}, e.what()); }
        });
    };
    result.copyText = [](const std::string& text) { auto pasteboard = NSPasteboard.generalPasteboard; [pasteboard clearContents]; if (![pasteboard setString:ns(text) forType:NSPasteboardTypeString]) throw std::runtime_error("Could not copy to clipboard"); };
    result.reveal = [](const fs::path& path) { NSURL* url = [NSURL fileURLWithPath:ns(utf8(path))]; [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[url]]; return Json{{"opened", true}, {"revealed", true}}; };
    result.openExternal = [](const std::string& url) { if (![NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:ns(url)]]) throw std::runtime_error("Could not open the default browser"); };
    return result;
}
}

@interface ABResources : NSObject <WKURLSchemeHandler>
@end
@implementation ABResources
- (void)webView:(WKWebView*)webView startURLSchemeTask:(id<WKURLSchemeTask>)task {
    (void)webView;
    try {
        auto resource = readUiResource(runtimePaths().ui, string(task.request.URL.absoluteString), Origin);
        if (!resource) { [task didFailWithError:[NSError errorWithDomain:@"AnimBridgeResourceDenied" code:403 userInfo:nil]]; return; }
        NSDictionary* headers = @{@"Content-Type": ns(resource->mime), @"Content-Security-Policy": ns(UiContentSecurityPolicy), @"X-Content-Type-Options": @"nosniff", @"Cache-Control": @"no-store"};
        NSHTTPURLResponse* response = [[NSHTTPURLResponse alloc] initWithURL:task.request.URL statusCode:200 HTTPVersion:@"HTTP/1.1" headerFields:headers];
        [task didReceiveResponse:response]; [task didReceiveData:[NSData dataWithBytes:resource->bytes.data() length:resource->bytes.size()]]; [task didFinish];
    } catch (...) { [task didFailWithError:[NSError errorWithDomain:@"AnimBridgeResourceFailure" code:500 userInfo:nil]]; }
}
- (void)webView:(WKWebView*)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task { (void)webView; (void)task; }
@end

@interface ABWebView : WKWebView
@end
@implementation ABWebView
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender { return [sender.draggingPasteboard canReadObjectForClasses:@[NSURL.class] options:@{NSPasteboardURLReadingFileURLsOnlyKey:@YES}] ? NSDragOperationCopy : NSDragOperationNone; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    if (closing || !bridge) return NO;
    try { std::vector<fs::path> paths; for (NSURL* url in [sender.draggingPasteboard readObjectsForClasses:@[NSURL.class] options:@{NSPasteboardURLReadingFileURLsOnlyKey:@YES}]) if (url.isFileURL) paths.push_back(fromUtf8(string(url.path))); if (paths.empty()) return NO; bridge->ingest(paths); return YES; }
    catch (const std::exception& e) { deliver({{"event", "error"}, {"data", messageData(e)}}); return NO; }
}
@end

@interface ABHost : NSObject <NSApplicationDelegate, NSWindowDelegate, WKNavigationDelegate, WKUIDelegate, WKScriptMessageHandler>
@end
@implementation ABHost
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    try {
        context = resolveRuntimeContext(dataOverride); ownership = std::make_unique<InstanceLock>(context.dataRoot);
        if (!ownership->acquired()) throw std::runtime_error(ownership->diagnostic());
        mainWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1480, 900) styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
#if defined(ANIMBRIDGE_ENABLE_UI_TEST_HOOKS) && ANIMBRIDGE_ENABLE_UI_TEST_HOOKS
        mainWindow.title = @"AnimBridge · G2 Harness";
#else
        mainWindow.title = @"AnimBridge";
#endif
        mainWindow.minSize = NSMakeSize(900, 640); mainWindow.delegate = self; [mainWindow center];
        WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
        [config setURLSchemeHandler:[[ABResources alloc] init] forURLScheme:@"animbridge"];
        [config.userContentController addScriptMessageHandler:self name:@"animbridge"];
        config.preferences.javaScriptCanOpenWindowsAutomatically = NO; config.websiteDataStore = WKWebsiteDataStore.defaultDataStore;
        browser = [[ABWebView alloc] initWithFrame:mainWindow.contentView.bounds configuration:config];
        browser.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable; browser.navigationDelegate = self; browser.UIDelegate = self;
#if defined(ANIMBRIDGE_ENABLE_UI_TEST_HOOKS) && ANIMBRIDGE_ENABLE_UI_TEST_HOOKS
        browser.inspectable = automation;
#else
        browser.inspectable = NO;
#endif
        [browser registerForDraggedTypes:@[NSPasteboardTypeFileURL]]; mainWindow.contentView = browser;
        bridge = std::make_unique<DesktopBridge>(deliver, services(), dataOverride);
        [mainWindow makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
        [browser loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:ns(Document)]]];
        std::signal(SIGTERM, SIG_IGN); terminationSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(terminationSource, ^{ terminating = true; if (bridge) bridge->cancel(); [NSApp terminate:nil]; }); dispatch_resume(terminationSource);
    } catch (const std::exception& e) { failure(e); [NSApp terminate:nil]; }
}
- (void)userContentController:(WKUserContentController*)controller didReceiveScriptMessage:(WKScriptMessage*)message {
    (void)controller;
    if (closing || !bridge || !message.frameInfo.mainFrame || ![message.body isKindOfClass:NSDictionary.class]) return;
    try {
        auto provenance = string(message.frameInfo.request.URL.absoluteString);
        if (![message.frameInfo.securityOrigin.protocol isEqualToString:@"animbridge"] || ![message.frameInfo.securityOrigin.host isEqualToString:@"ui"]) return;
        NSData* encoded = [NSJSONSerialization dataWithJSONObject:message.body options:0 error:nil]; if (!encoded || encoded.length > 1024 * 1024) return;
        auto request = Json::parse(std::string(static_cast<const char*>(encoded.bytes), encoded.length));
        if (!transport.accept(request, provenance, true, true)) return;
        if (request.value("action", "") == "window") { deliver({{"id", request.at("id")}, {"ok", true}, {"result", {{"nativeDragRegions", true}, {"customFrame", false}}}}); return; }
        bridge->handle(request);
    } catch (const std::exception& e) { deliver({{"event", "error"}, {"data", messageData(e)}}); }
}
- (void)webView:(WKWebView*)webView decidePolicyForNavigationAction:(WKNavigationAction*)action decisionHandler:(void (^)(WKNavigationActionPolicy))done {
    (void)webView;
    // Block frames and external documents before any commit/bootstrap.
    BOOL allowed = !action.shouldPerformDownload && action.targetFrame && action.targetFrame.mainFrame && [action.request.URL.absoluteString isEqualToString:ns(Document)];
    if (allowed) transport.invalidate(); done(allowed ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}
- (void)webView:(WKWebView*)webView decidePolicyForNavigationResponse:(WKNavigationResponse*)response decisionHandler:(void (^)(WKNavigationResponsePolicy))done {
    (void)webView;
    BOOL allowed = response.forMainFrame && response.canShowMIMEType && [response.response.URL.absoluteString isEqualToString:ns(Document)];
    done(allowed ? WKNavigationResponsePolicyAllow : WKNavigationResponsePolicyCancel);
}
- (void)webView:(WKWebView*)webView didStartProvisionalNavigation:(WKNavigation*)navigation { (void)webView; (void)navigation; transport.invalidate(); }
- (void)webView:(WKWebView*)webView didFailNavigation:(WKNavigation*)navigation withError:(NSError*)error { (void)webView; (void)navigation; (void)error; transport.invalidate(); }
- (void)webView:(WKWebView*)webView didFailProvisionalNavigation:(WKNavigation*)navigation withError:(NSError*)error { (void)webView; (void)navigation; (void)error; transport.invalidate(); }
- (void)webViewWebContentProcessDidTerminate:(WKWebView*)webView { (void)webView; transport.invalidate(); }
- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
    (void)navigation; if (closing || ![webView.URL.absoluteString isEqualToString:ns(Document)]) return;
    try {
        auto bootstrap = transport.commit(Document, "macos", false);
        [webView evaluateJavaScript:ns("window.__animbridgeReceive?.(" + bootstrap.dump() + ")") completionHandler:nil];
        if (!documents.empty()) { auto paths = std::move(documents); documents.clear(); bridge->ingest(paths, GrantSource::NativeDocument); }
    } catch (const std::exception& e) { deliver({{"event", "error"}, {"data", messageData(e)}}); }
}
- (WKWebView*)webView:(WKWebView*)webView createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration forNavigationAction:(WKNavigationAction*)action windowFeatures:(WKWindowFeatures*)features { (void)webView; (void)configuration; (void)action; (void)features; return nil; }
- (void)webView:(WKWebView*)webView requestMediaCapturePermissionForOrigin:(WKSecurityOrigin*)origin initiatedByFrame:(WKFrameInfo*)frame type:(WKMediaCaptureType)type decisionHandler:(void (^)(WKPermissionDecision))done { (void)webView; (void)origin; (void)frame; (void)type; done(WKPermissionDecisionDeny); }
- (void)webView:(WKWebView*)webView runOpenPanelWithParameters:(WKOpenPanelParameters*)parameters initiatedByFrame:(WKFrameInfo*)frame completionHandler:(void (^)(NSArray<NSURL*>*))done { (void)webView; (void)parameters; (void)frame; done(nil); }
- (void)application:(NSApplication*)application openURLs:(NSArray<NSURL*>*)urls { (void)application; try { std::vector<fs::path> paths; for (NSURL* url in urls) if (url.isFileURL) paths.push_back(fromUtf8(string(url.path))); if (bridge && transport.ready()) bridge->ingest(paths, GrantSource::NativeDocument); else documents.insert(documents.end(), paths.begin(), paths.end()); } catch (const std::exception& e) { failure(e); } }
- (BOOL)windowShouldClose:(NSWindow*)window { (void)window; return stopWindow(); }
- (void)windowWillClose:(NSNotification*)notification { (void)notification; [NSApp terminate:nil]; }
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender { (void)sender; return YES; }
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender { (void)sender; return stopWindow() ? NSTerminateNow : NSTerminateCancel; }
@end

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            clearBrowserEnvironmentOverrides();
            for (int i = 1; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--data-dir" && i + 1 < argc) dataOverride = fs::absolute(fromUtf8(argv[++i]));
#if defined(ANIMBRIDGE_ENABLE_UI_TEST_HOOKS) && ANIMBRIDGE_ENABLE_UI_TEST_HOOKS
                else if (arg == "--ui-test" || arg == "--automation") automation = true;
#endif
                else if (arg.starts_with("--")) throw std::runtime_error("Unknown AnimBridge option");
                else if (!arg.starts_with("-psn_")) documents.push_back(fs::absolute(fromUtf8(arg)));
            }
            NSApplication* app = NSApplication.sharedApplication; [app setActivationPolicy:NSApplicationActivationPolicyRegular];
            NSMenu* menu = [[NSMenu alloc] init]; NSMenuItem* root = [[NSMenuItem alloc] init]; [menu addItem:root];
            NSMenu* appMenu = [[NSMenu alloc] init]; [appMenu addItemWithTitle:@"Quit AnimBridge" action:@selector(terminate:) keyEquivalent:@"q"]; root.submenu = appMenu;
            NSMenuItem* edit = [[NSMenuItem alloc] initWithTitle:@"Edit" action:nil keyEquivalent:@""]; NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
            for (NSArray* command in @[@[@"Undo", @"undo:", @"z"], @[@"Cut", @"cut:", @"x"], @[@"Copy", @"copy:", @"c"], @[@"Paste", @"paste:", @"v"], @[@"Select All", @"selectAll:", @"a"]]) [editMenu addItemWithTitle:command[0] action:NSSelectorFromString(command[1]) keyEquivalent:command[2]];
            edit.submenu = editMenu; [menu addItem:edit]; app.mainMenu = menu;
            ABHost* delegate = [[ABHost alloc] init]; app.delegate = delegate; [app run];
            closing = true; transport.invalidate(); bridge.reset(); ownership.reset(); if (terminationSource) dispatch_source_cancel(terminationSource); return 0;
        } catch (const std::exception& e) { failure(e); return 1; }
    }
}
