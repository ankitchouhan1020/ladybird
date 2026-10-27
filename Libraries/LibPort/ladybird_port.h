/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The C ABI between LibWebView and a non-C++ UI.
 *
 * This header is PURE C on purpose (contract rule R2). It declares no C++ types, no templates, no objc_msgSend, and
 * nothing that needs Swift/C++ interop: a Swift client imports it as a plain C module and needs no
 * -enable-experimental-cxx-interop. CoreFoundation is a C API, so the run loop timer the shim installs is C too.
 *
 * The shim owns the engine and never blocks. The host (SwiftUI, AppKit, anything) owns the main run loop and calls
 * LbAppPump, or calls LbAppStartPump and lets the shim install a CFRunLoopTimer that pumps for it.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

// Reserved so a host does not have to guess: ordinal 0 in Web::UIEvents::KeyCode is the invalid key.
#define LbKeyInvalid 0

// The `version` every size-tagged callback struct carries. A registration function returns 0 when the caller's
// version is not this one, so a host newer than the dylib loses the feature instead of being misread.
#define LB_CALLBACKS_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LbApp LbApp;

typedef struct LbCallbacks {
    void* context;

    // A new frame is available for `tab`. Demand-driven: a settled page stops producing these (contract rule R3).
    // Called on the main thread, from inside the pump, on the run loop that LbAppStartPump or the host drives.
    void (*on_ready_to_paint)(void* context, uint64_t tab);

    // The tab's displayed URL changed. `url` is not NUL-terminated; `len` gives its length. Only valid for this call.
    void (*on_url_change)(void* context, uint64_t tab, const char* url, size_t len);

    // A navigation in `tab` finished loading. Same `url`/`len` lifetime rules as above.
    void (*on_load_finish)(void* context, uint64_t tab, const char* url, size_t len);

    // The tab's document title changed. `title` is UTF-8 encoded and not NUL-terminated; `len` gives its length. Valid for this call only.
    void (*on_title_change)(void* context, uint64_t tab, const char* title, size_t len);

    // The tab's loading state changed: 1 if loading, 0 if idle/finished.
    void (*on_loading_state_change)(void* context, uint64_t tab, int is_loading);

    // The tab's favicon changed. If `data` is NULL or `len` is 0, the favicon was cleared.
    // When present, `data` points to PNG encoded bytes, and `hash` is an ASCII hex SHA-256 string (not NUL-terminated; `hash_len` gives its length).
    // Valid for this call only.
    void (*on_favicon_change)(void* context, uint64_t tab, const uint8_t* data, size_t len, const char* hash, size_t hash_len);

    // ---------------------------------------------------------------------------------------------------------------
    // Modal dialogs. These are the only callbacks the engine BLOCKS on: the page's script stops until one is answered,
    // and the freeze is in WebContent, so a host that renders correctly but never answers looks alive while the page is
    // wedged. Every one of these MUST be answered, with the matching LbTabAnswer* call.
    //
    // Worst case, present-but-silent hangs the page: installing a callback and not answering it wedges the tab forever,
    // because nothing else will.
    //
    // Absent is NOT safe, which is what this paragraph used to claim. The claim holds for a fresh ViewImplementation,
    // but this shim's view derives from WebView::HeadlessWebView, whose constructor already installed alert/confirm/
    // prompt handlers that only record Web::PendingDialog and never answer. An absent host callback used to leave those
    // recording handlers in place, and Page::did_request_alert parks the page in spin_event_loop_until_dialog_closed
    // until an answer arrives — so a host that installed no dialogs got a permanently wedged tab, not a self-answering
    // engine. See Documentation/Traps.md, "An absent dialog callback does NOT make the engine answer itself".
    //
    // So the shim now always installs its own handler for every one of these four, whether or not the host supplied a
    // callback. With a callback it hands the request over; without one it answers unavailable immediately (alert
    // closes, confirm declines, prompt cancels, before_unload allows — the same outcomes the header always promised).
    // The host may therefore install any subset of the four. Every callback group added below follows the same rule.
    //
    // `message` is not NUL-terminated; `len` gives its length and is valid for this call only.
    // `default_text` on the prompt is the value the field starts with, same lifetime rules.
    // ---------------------------------------------------------------------------------------------------------------

    // alert(). Answer with LbTabAnswerAlert.
    void (*on_request_alert)(void* context, uint64_t tab, const char* message, size_t len);

    // confirm(). Answer with LbTabAnswerConfirm. Declining is the safe default.
    void (*on_request_confirm)(void* context, uint64_t tab, const char* message, size_t len);

    // prompt(). Answer with LbTabAnswerPrompt.
    void (*on_request_prompt)(void* context, uint64_t tab, const char* message, size_t len, const char* default_text, size_t default_len);

    // beforeunload. Answer with LbTabAnswerBeforeUnload. NOTE THE INVERTED SENSE: `accept` of 1 means ALLOW the
    // navigation, not block it — the opposite of what the word suggests, and the bool has already misled one careful
    // reader. A page that returns false from its beforeunload handler should be answered 0.
    void (*on_request_before_unload)(void* context, uint64_t tab, const char* source, size_t len);
} LbCallbacks;

// ---------------------------------------------------------------------------------------------------------------
// Callback groups added after LbCallbacks.
//
// Each concern gets its own struct and its own registration function, because LbCallbacks' layout is baked into a
// host's compiled-in field offsets: widening it would make an older host run against a newer dylib a SIGSEGV at
// startup rather than a stale build. The first two members of every struct below are exactly `struct_size` and
// `version`, so the dylib can tell a struct it does not understand from one it does.
//
// A struct's first two members are the only fields a host MUST set; every registration function validates them.
// ---------------------------------------------------------------------------------------------------------------

// What on_cursor_change reports for an image cursor: `standard_cursor` is LbCursorImage and `image_png`/`image_png_len`
// carry a PNG-encoded bitmap with the hotspot to tip it at. Every other value of `standard_cursor` is a
// Gfx::StandardCursor ordinal — the engine's own values, not a renumbering here (Libraries/LibGfx/Cursor.h is the
// source of truth): None = 0, Hidden = 1, Arrow = 2, Crosshair = 3, IBeam = 4, ResizeHorizontal = 5,
// ResizeVertical = 6, ResizeDiagonalTLBR = 7, ResizeDiagonalBLTR = 8, ResizeColumn = 9, ResizeRow = 10, Hand = 11,
// Help = 12, OpenHand = 13, Drag = 14, DragCopy = 15, Move = 16, Wait = 17, Disallowed = 18, Eyedropper = 19,
// Zoom = 20.
#define LbCursorImage (-1)

// Cursor, link-hover and tooltip reporting: everything a host needs for hover feedback that is not a click.
typedef struct LbUiCallbacks {
    uint32_t struct_size;
    uint32_t version;

    // The cursor the page wants under the pointer. `image_png` is NULL unless `standard_cursor` is LbCursorImage, and
    // both the bytes and the hotspot are valid for this call only.
    void (*on_cursor_change)(void* context, uint64_t tab, int32_t standard_cursor, const uint8_t* image_png, size_t image_png_len, int32_t hotspot_x, int32_t hotspot_y);

    // The URL under the pointer changed; `url` is UTF-8 and not NUL-terminated. Valid for this call only.
    void (*on_link_hover)(void* context, uint64_t tab, const char* url, size_t len);

    // The pointer left the link it was over, or the hovered link went away. There is no URL: the label is cleared.
    void (*on_link_unhover)(void* context, uint64_t tab);

    // Show `text` as a tooltip at (x, y) — CSS pixels from the tab's top-left, same space as LbTabSendMouse — instead
    // of wherever a hover timer would put one. `text` is UTF-8, not NUL-terminated, valid for this call only.
    void (*on_request_tooltip_override)(void* context, uint64_t tab, int32_t x, int32_t y, const char* text, size_t len);

    // Stop showing the tooltip an on_request_tooltip_override asked for. Leave `title=` tooltips to on_enter_tooltip_area.
    void (*on_stop_tooltip_override)(void* context, uint64_t tab);

    // The pointer entered an element with a tooltip (a `title=` attribute, typically). `text` is UTF-8.
    void (*on_enter_tooltip_area)(void* context, uint64_t tab, const char* text, size_t len);

    // The pointer left a tooltip area, or the element's tooltip went away.
    void (*on_leave_tooltip_area)(void* context, uint64_t tab);
} LbUiCallbacks;

// Page lifecycle, crash reporting and find-in-page. Everything here is a notification: none of it blocks the page, so
// a host may leave any of them NULL.
typedef struct LbPageCallbacks {
    uint32_t struct_size;
    uint32_t version;

    // A navigation in `tab` started. `on_load_finish` in LbCallbacks is the other half.
    void (*on_load_start)(void* context, uint64_t tab);

    // A top-level navigation's document committed in `tab`: the address it shows is now the document it runs.
    void (*on_top_level_navigation_commit)(void* context, uint64_t tab);

    // A back/forward traversal finished in `tab`, whether it committed or was cancelled.
    void (*on_history_traversal_complete)(void* context, uint64_t tab);

    // The renderer process for `tab` died. `reason` is a WebView::ViewImplementation::WebContentCrashReason ordinal:
    // 0 = ProcessCrash, 1 = RejectedIPC. Without this a crashed tab keeps showing its last frame with no explanation.
    void (*on_web_content_crashed)(void* context, uint64_t tab, int32_t reason);

    // The crash overlay should be shown (1) or hidden (0) over `tab`.
    void (*on_crash_overlay_state_change)(void* context, uint64_t tab, int active);

    // Find-in-page progress. `current_match_index` is zero-based. `has_total_match_count` is 0 when the engine does not
    // (yet) know how many matches there are, and `total_match_count` is 0 in that case.
    void (*on_find_in_page)(void* context, uint64_t tab, size_t current_match_index, int has_total_match_count, size_t total_match_count);

    // `tab` opened another tab or window — `window.open()`, `target="_blank"`, a middle-click. The shim has already
    // created `new_tab`, registered it as an LbTab of its own and pointed it at the engine's new page, so the host can
    // call LbTabGetSurface/LbTabSetViewport on it immediately; `new_tab` is the id to use, never 0. `activate_tab` is 1
    // when the page asked for the new tab to be focused. `is_popup` is 1 for a window opened with window features and 0
    // for an ordinary new tab. A host that ignores this leaks the view until the app quits.
    void (*on_new_tab)(void* context, uint64_t parent_tab, uint64_t new_tab, int activate_tab, int is_popup);

    // `tab` closed itself (window.close(), or the engine discarding it). The shim has already unregistered it, so the
    // host must drop it without calling LbTabDestroy — that would be a no-op on an id that no longer exists.
    void (*on_tab_closed)(void* context, uint64_t tab);
} LbPageCallbacks;

// Everything the engine asks the host to put a picker or a prompt on screen for. Each callback carries a
// `request_id` that must be answered exactly once with the matching LbTabAnswer* call.
//
// Item B8 of Documentation/Reports/Bridge-Gap-Inventory.md applies to every one of these: an uninstalled callback
// used to leave an <select> permanently open and a file input permanently waiting, so the shim installs its own
// handler for all five regardless and answers unavailable/cancelled immediately when the host supplied no callback.
// The host may therefore install any subset, and a page never waits on a picker nobody will show.
//
// Structured payloads cross as JSON text, because they are Vector/Variant graphs with no C shape. Both are UTF-8 and
// not NUL-terminated, and valid for this call only.
typedef struct LbPickerCallbacks {
    uint32_t struct_size;
    uint32_t version;

    // A <select> wants its dropdown at (x, y) — CSS pixels, the same space as LbTabSendMouse — at least
    // `minimum_width` CSS pixels wide. `items_json` is an array in document order, matching Web::HTML::SelectItem:
    //   {"type":"option","id":N,"label":"...","enabled":bool,"selected":bool} — N is the id to pass back in LbTabAnswerSelectDropdown
    //   {"type":"separator"}                                     — no other members
    //   {"type":"group","label":"...","items":[option...]}     — a group's items are options only, never nested groups
    // Answer with LbTabAnswerSelectDropdown.
    void (*on_request_select_dropdown)(void* context, uint64_t tab, uint64_t request_id, int32_t x, int32_t y, int32_t minimum_width, const char* items_json, size_t items_len);

    // An <input type="file"> wants a file chooser. `filters_json` is an array matching Web::HTML::FileFilter:
    //   {"type":"audio"} | {"type":"image"} | {"type":"video"}   — a whole kind, no `value`
    //   {"type":"mime","value":"image/png"}
    //   {"type":"extension","value":"png"}                    — no leading dot
    // `allow_multiple` is 1 when the input accepts more than one file. Answer with LbTabAnswerFilePicker.
    void (*on_request_file_picker)(void* context, uint64_t tab, uint64_t request_id, const char* filters_json, size_t filters_len, int allow_multiple);

    // An <input type="color"> wants a colour picker, starting from rgba. Answer with LbTabAnswerColorPicker.
    void (*on_request_color_picker)(void* context, uint64_t tab, uint64_t request_id, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

    // The page wants a permission (today only "microphone"). `name` and `origin` are UTF-8, not NUL-terminated;
    // `origin` is the serialization of the requesting document's origin. Answer with LbTabAnswerPermission.
    // Pointer-then-length, the same order as every other string in this header.
    void (*on_request_permission)(void* context, uint64_t tab, uint64_t request_id, const char* name, size_t name_len, const char* origin, size_t origin_len);

    // The page navigated somewhere an application outside the browser would have to open. Ask the user; answer with
    // LbTabAnswerExternalURL, where accepted means "open it", not "rejected".
    void (*on_request_external_url_confirmation)(void* context, uint64_t tab, uint64_t request_id, const char* url, size_t url_len, const char* origin, size_t origin_len);
} LbPickerCallbacks;

// A frame that is ready to display. The handle is a borrowed IOSurfaceRef: the caller must CFRetain it to keep it
// across calls. The pixels are never copied or cropped here (contract rule R1), so `painted_width`/`painted_height`
// may be smaller than the surface itself; display the painted part with CALayer.contentsRect.
typedef struct LbSurface {
    void* iosurface;
    int32_t painted_width;
    int32_t painted_height;
    int32_t surface_width;
    int32_t surface_height;
} LbSurface;

// Creates the engine. `argv0` is only used for logging and resource lookup.
LbApp* LbAppCreate(const char* argv0, const LbCallbacks* callbacks);

//
// Callback-group registration. Each returns 1 when applied and 0 when it was refused — a dylib that predates the group
// has no such symbol at all, and one that does refuses a struct it cannot trust. A refused group costs the host that
// feature and never a crash.
//
// Refused when: `callbacks` is NULL, `struct_size` is smaller than the struct this dylib expects, or `version` is not
// LB_CALLBACKS_VERSION.
//
// The callbacks apply to every tab, including ones created before the call: the handlers are installed when a tab is
// created and read the current group at dispatch time. Register before or after LbTabCreate, either works.
//
int LbAppSetUiCallbacks(LbApp* app, const LbUiCallbacks* callbacks);
int LbAppSetPageCallbacks(LbApp* app, const LbPageCallbacks* callbacks);
int LbAppSetPickerCallbacks(LbApp* app, const LbPickerCallbacks* callbacks);

// Which system theme subsequently created tabs load: 0 = Base/res/themes/Default.ini, non-zero =
// Base/res/themes/Dark.ini. Tabs that already exist are unaffected.
//
// It has to be chosen at creation because WebView::HeadlessWebView::system_theme() hands out the Core::AnonymousBuffer
// its constructor was given, so the theme is fixed for a headless view's lifetime. A host that wants an existing tab to
// change appearance asks the page (LbTabSetColorScheme) rather than the theme.
void LbAppSetTheme(LbApp* app, int dark);

// One non-blocking iteration of the engine's event loop (Core::EventLoop::WaitMode::PollForEvents).
void LbAppPump(LbApp* app);

// Installs a CFRunLoopTimer on the current run loop that calls LbAppPump for every tick. The host then runs its own
// loop; this is the shape a SwiftUI app wants, since a blocking LbAppRun would fight it.
void LbAppStartPump(LbApp* app, double interval_seconds);

// Stops the timer LbAppStartPump installed, if any.
void LbAppStopPump(LbApp* app);

// Tears the engine down. Must be called from the main thread, after LbAppStopPump.
void LbAppDestroy(LbApp* app);

uint64_t LbTabCreate(LbApp* app);

// Names the tab receiving native input. UI-initiated new-tab requests such as Command-click use it as their parent.
void LbAppSetActiveTab(LbApp* app, uint64_t tab);

// Destroys one tab and its engine page. The host must stop asking for its surface first: a get-surface after this
// returns 0 rather than a stale frame, but presenting a frame that arrived before the call would be a use-after-free.
// Every other tab is unaffected, and the app outlives all of them.
void LbTabDestroy(LbApp* app, uint64_t tab);

void LbTabLoad(LbApp* app, uint64_t tab, const char* url, size_t len);

// What the address bar's text means, resolved by the engine rather than the host.
//
// A host cannot do this correctly on its own: deciding whether "ladybird" is a host or a search term needs the public
// suffix list and the user's configured search engine, and a second implementation would drift from the browser's real
// behaviour on the next upstream sync. This is WebView::sanitize_url -- the same call the Qt frontend makes -- so both
// frontends agree on what a given input navigates to.
//
// Returns 1 and fills the out parameters with an allocated UTF-8 URL on success, 0 if the input could be resolved
// neither as a URL nor as a search. The caller owns the buffer and must release it with LbStringFree. An empty search
// engine means a bare word that is not a URL returns 0, which is how a host detects "no engine configured".
int LbResolveUserInput(const char* utf8, size_t len, char** out_url, size_t* out_len);

// Releases a buffer returned by LbResolveUserInput. Null-safe, and does nothing for a null pointer.
void LbStringFree(char* string);

// Fills `out` with the tab's current frame. Returns 1 if a frame was written, 0 if the tab has none yet.
int LbTabGetSurface(LbApp* app, uint64_t tab, LbSurface* out);

// The viewport in CSS pixels plus the display's device pixel ratio. Call on creation and whenever the window resizes.
void LbTabSetViewport(LbApp* app, uint64_t tab, int32_t width, int32_t height, double device_pixel_ratio);

//
// Session history.
//
// `delta` is -1 for back and +1 for forward. Any other value is clamped to those two: the ABI exposes the two buttons a
// browser has, not the engine's arbitrary-step traversal, so a host cannot ask for a jump it has no UI for.
//
// Both are safe on a closed tab (they no-op) and safe to call before the first navigation.
void LbTabTraverseHistory(LbApp* app, uint64_t tab, int32_t delta);

// Whether `LbTabTraverseHistory` would do anything right now: 1 if yes, 0 if no. Use it to enable or disable the button.
//
// This reads the engine's own `navigate_back_action` / `navigate_forward_action`, which LibWebView maintains, rather
// than deriving "is there anything behind us" from step counters. Reimplementing that rule in a host is exactly the
// kind of logic that silently drifts from the engine on an upstream change.
int LbTabCanTraverseHistory(LbApp* app, uint64_t tab, int32_t delta);

//
// Input.
//
// Positions are in CSS pixels relative to the tab's top-left, which is the same space as the viewport. The shim scales
// them by the device pixel ratio, because the engine works in device pixels.
//
// These deliberately mirror the small set of fields a browser actually sets, rather than exposing Web::MouseEvent. That
// struct has designated-default fields a host would silently leave unset: `should_insert_text` is the documented one —
// omit it and the key is delivered, handled, and inserts nothing, with no error anywhere. Filling those in is the shim's
// job, so the host states intent ("a left click at x,y") and not transport.

// Matches Web::UIEvents::MouseButton, which is a bitmask. `Primary` is the left button.
typedef enum LbMouseButton {
    LbMouseButtonNone = 0,
    LbMouseButtonPrimary = 1,
    LbMouseButtonSecondary = 2,
    LbMouseButtonMiddle = 4,
} LbMouseButton;

typedef enum LbMouseEventType {
    LbMouseEventDown = 0,
    LbMouseEventUp = 1,
    LbMouseEventMove = 2,
    LbMouseEventWheel = 3,
} LbMouseEventType;

// `button` is the button this event concerns; `buttons` is every button held during it. A button-up carries the still-held
// set, which is why they are separate arguments. For a wheel event the buttons are ignored and `wheel_delta_y` is the
// pixel delta (positive scrolls down). `modifiers` is the Web::UIEvents::KeyModifier bitmask; without it modifier-click
// navigation such as Command-click cannot work.
void LbTabSendMouseWithModifiers(LbApp* app, uint64_t tab, LbMouseEventType type, double x, double y, LbMouseButton button, LbMouseButton buttons, int32_t click_count, double wheel_delta_y, uint32_t modifiers);

// Compatibility entry point for hosts built before mouse modifiers crossed the boundary.
void LbTabSendMouse(LbApp* app, uint64_t tab, LbMouseEventType type, double x, double y, LbMouseButton button, LbMouseButton buttons, int32_t click_count, double wheel_delta_y);

// A single key transition. `key` is a Web::UIEvents::KeyCode ordinal; the host passes `LbKeyInvalid` (0) when it only has
// text, and the shim then leaves the key unresolved. `code_point` is the character to insert (0 for a non-text key such
// as Tab or an arrow). `modifiers` is the Web::UIEvents::KeyModifier bitmask.
//
// Note the engine dispatches keys asynchronously, through the compositor. Returning from this function means the event was
// queued, NOT that the page handled it; there is no completion callback yet (see Documentation/Traps.md).
void LbTabSendKey(LbApp* app, uint64_t tab, int32_t key, uint32_t code_point, uint32_t modifiers, int is_key_down);

// Convenience for the common case: types `utf8` as text, one key transition pair per code point. This is what an address
// bar or a `<input>` needs; use LbTabSendKey directly for keys that are not text.
void LbTabTypeText(LbApp* app, uint64_t tab, const char* utf8, size_t len);

// Dialogs: the answers to the four blocking callbacks above. Each MUST be called exactly once for the dialog it
// answers, or the page stays wedged. Answering a tab that has no dialog pending is a no-op, so a host that closes a tab
// with a dialog up can simply answer and then close.
void LbTabAnswerAlert(LbApp* app, uint64_t tab);

// `accept` of 0 declines, which is the safe default.
void LbTabAnswerConfirm(LbApp* app, uint64_t tab, int accept);

// `response` of NULL is Cancel (equivalent to dismissing the prompt). An empty string is "accepted with no text", which
// is a different outcome, so the two are kept distinct rather than folded together.
void LbTabAnswerPrompt(LbApp* app, uint64_t tab, const char* response, size_t len);

// `accept` of 1 ALLOWS the navigation. See the inverted-sense note on on_request_before_unload.
void LbTabAnswerBeforeUnload(LbApp* app, uint64_t tab, int accept);

//
// Picker answers: one per pending LbPickerCallbacks request. Answering a `request_id` that is not pending on that tab
// (already answered, wrong tab, stale) is a no-op, so a host closing a tab with a picker up can answer and then close.
//

// `has_selection` of 0 means the dropdown was dismissed with nothing chosen, which is a different outcome from choosing
// the option whose id is 0 — there is no such id, options are numbered from 1.
void LbTabAnswerSelectDropdown(LbApp* app, uint64_t tab, uint64_t request_id, int has_selection, uint32_t selected_item_id);

// `json_or_null` is either NULL (the chooser was cancelled, no files selected) or a JSON array of absolute file paths,
// UTF-8, `len` bytes. The shim opens each path and hands the engine the files; a path it cannot open is skipped.
void LbTabAnswerFilePicker(LbApp* app, uint64_t tab, uint64_t request_id, const char* json_or_null, size_t len);

// `has_color` of 1 accepts the colour and closes the picker; 0 closes it with no colour chosen.
void LbTabAnswerColorPicker(LbApp* app, uint64_t tab, uint64_t request_id, int has_color, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

// `granted` of 0 denies, which is the safe default and the one used when the host installed no callback.
void LbTabAnswerPermission(LbApp* app, uint64_t tab, uint64_t request_id, int granted);

// `accepted` of 1 opens the URL in the other application; 0 declines. Note the sense: 1 means "open it", not "accept
// the block". Declining lets the engine move on to the next queued external URL.
void LbTabAnswerExternalURL(LbApp* app, uint64_t tab, uint64_t request_id, int accepted);

//
// Commands: engine methods Qt's frontend calls from its own UI. Each is a no-op on an unknown tab.
//

// Reloads `tab` as a reload rather than as a fresh navigation: the engine marks the history visit a reload, so
// on_before_browser_initiated_navigation fires and a navigation stopped before its document committed is retried.
// `bypass_cache` of 1 re-fetches from the network instead of the cache.
void LbTabReload(LbApp* app, uint64_t tab, int bypass_cache);

// Stops the current navigation, leaving the document that was showing.
void LbTabStopLoading(LbApp* app, uint64_t tab);

// Page zoom, as a multiplier: 1.0 is 100%. Out-of-range values are clamped by the engine.
void LbTabSetZoom(LbApp* app, uint64_t tab, double zoom_level);
void LbTabZoomIn(LbApp* app, uint64_t tab);
void LbTabZoomOut(LbApp* app, uint64_t tab);

// The tab's current zoom multiplier. 1.0 for an unknown tab, so a host can multiply unconditionally.
double LbTabZoomLevel(LbApp* app, uint64_t tab);

// The values for the three preference setters. They are this header's own numbering, not the engine enums', because
// the engine's order differs (auto, dark, light) and a host should not have to know that.
typedef enum LbPreferredColorScheme {
    LbPreferredColorSchemeAuto = 0,
    LbPreferredColorSchemeLight = 1,
    LbPreferredColorSchemeDark = 2,
} LbPreferredColorScheme;

typedef enum LbPreferredMotion {
    LbPreferredMotionAuto = 0,
    LbPreferredMotionNoPreference = 1,
    LbPreferredMotionReduce = 2,
} LbPreferredMotion;

typedef enum LbPreferredContrast {
    LbPreferredContrastAuto = 0,
    LbPreferredContrastLess = 1,
    LbPreferredContrastMore = 2,
    LbPreferredContrastNoPreference = 3,
} LbPreferredContrast;

// What the page's `prefers-color-scheme` resolves to. This is the half of dark mode a host owns: LbAppSetTheme picks
// the engine's colours, this picks the page's.
void LbTabSetColorScheme(LbApp* app, uint64_t tab, int32_t color_scheme);

// `prefers-reduced-motion`.
void LbTabSetPreferredMotion(LbApp* app, uint64_t tab, int32_t motion);

// `prefers-contrast`.
void LbTabSetPreferredContrast(LbApp* app, uint64_t tab, int32_t contrast);

// `visible` of 0 tells the engine the tab is in a background window, which throttles its timers and makes its documents'
// `document.visibilityState` "hidden". Set it back to 1 when the tab is shown again.
void LbTabSetSystemVisibility(LbApp* app, uint64_t tab, int visible);

// `focused` of 0 tells the engine another window has the keyboard, which the page sees through `document.hasFocus()`.
void LbTabSetSystemFocus(LbApp* app, uint64_t tab, int focused);

// Mutes or unmutes every audio source in the tab.
void LbTabToggleMute(LbApp* app, uint64_t tab);

// 1 when the tab is muted, 0 when it is not, and 0 for an unknown tab.
int LbTabPageMuteState(LbApp* app, uint64_t tab);

// The editing commands the engine's own Edit menu drives. These exist so ⌘A / ⌘Z reach the page even when the host
// routes the shortcut through its menu rather than through LbTabSendKey.
void LbTabSelectAll(LbApp* app, uint64_t tab);
void LbTabUndo(LbApp* app, uint64_t tab);
void LbTabRedo(LbApp* app, uint64_t tab);

// Find in page. `query` is UTF-8 and not NUL-terminated. `match_case` of 0 is case-insensitive (the browser default),
// and `highlight_all` of 1 highlights every match instead of only the current one. Progress arrives through
// LbPageCallbacks::on_find_in_page.
void LbTabFindInPage(LbApp* app, uint64_t tab, const char* utf8, size_t len, int match_case, int highlight_all);
void LbTabFindNextMatch(LbApp* app, uint64_t tab);
void LbTabFindPreviousMatch(LbApp* app, uint64_t tab);

// Ends find-in-page and clears the highlights. LbTabFindInPage starts it again.
void LbTabFindEnd(LbApp* app, uint64_t tab);

#ifdef __cplusplus
}
#endif
