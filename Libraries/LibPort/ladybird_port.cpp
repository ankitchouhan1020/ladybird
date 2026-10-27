/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The bridge between LibWebView and a non-C++ UI. Plain C++ and CoreFoundation only: no ObjC, no CALayer, no NSWindow.
 * The host owns all of that, and per contract rule R2 the surface crosses as a bare IOSurfaceRef.
 */

#include "ladybird_port.h"

#include <AK/HashMap.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/RefCounted.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <AK/Utf8View.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/Cursor.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/CSS/PreferredColorScheme.h>
#include <LibWebCommon/CSS/PreferredContrast.h>
#include <LibWebCommon/CSS/PreferredMotion.h>
#include <LibWebCommon/HTML/AudioPlayState.h>
#include <LibWebCommon/HTML/ColorPickerUpdateState.h>
#include <LibWebCommon/HTML/FileFilter.h>
#include <LibWebCommon/HTML/SelectItem.h>
#include <LibWebCommon/HTML/SelectedFile.h>
#include <LibWebCommon/HTML/VisibilityState.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/UIEvents/KeyCode.h>
#include <LibWebCommon/UIEvents/MouseButton.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/FaviconStore.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Menu.h>
#include <LibWebView/SearchEngine.h>
#include <LibWebView/Settings.h>
#include <LibWebView/URL.h>
#include <LibWebView/Utilities.h>
#include <stdlib.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>

namespace {

class PortApplication final : public WebView::Application {
    WEB_VIEW_APPLICATION(PortApplication)

public:
    explicit PortApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions&) override
    {
        // The host owns the event loop and the window, so the engine must not run headless or coordinate a browser.
        browser_options.headless_mode.clear();
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;

        // NOT test mode, which the probes use. Test mode is a layout-test switch, not a sandbox: it makes
        // FontPlugin::generic_font_name() return "SerenitySans" for every generic family, skipping font matching
        // entirely, so every page renders in the bundled fallback. That is correct for a probe that must produce
        // identical pixels everywhere and wrong for a browser — it was the reason this app ignored system fonts.
        // Left at the default (No).
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
    virtual Optional<WebView::ViewImplementation&> open_blank_new_tab(Web::HTML::ActivateTab) const override;

    void set_port_app(LbApp* app) { m_port_app = app; }

private:
    LbApp* m_port_app { nullptr };
};

// The bitmap members are protected, so a subclass is the sanctioned access path. This is current_paintable() from
// UI/Qt/WebContentView.cpp, gate included: without the m_has_usable_bitmap check a bitmap that is mid-swap would be
// presented as if it were the front one.
class PortWebView final : public WebView::HeadlessWebView {
public:
    static NonnullOwnPtr<PortWebView> create(Core::AnonymousBuffer theme, Web::DevicePixelSize window_size)
    {
        auto view = adopt_own(*new PortWebView(move(theme), window_size));
        view->initialize_tab(Web::HTML::VisibilityState::Visible);
        return view;
    }

    struct Frame {
        Gfx::SharedImageBuffer const* buffer { nullptr };
        Gfx::IntSize painted_size;
    };

    Optional<Frame> current_frame() const
    {
        Gfx::SharedImageBuffer const* buffer = nullptr;
        Gfx::IntSize size;

        if (m_has_usable_bitmap) {
            VERIFY(m_front_bitmap.shared_image_buffer);
            buffer = m_front_bitmap.shared_image_buffer.ptr();
            size = m_front_bitmap.last_painted_size.to_type<int>();
        } else if (m_backup_shared_image_buffer) {
            buffer = m_backup_shared_image_buffer.ptr();
            size = m_backup_bitmap_size.to_type<int>();
        }

        if (!buffer)
            return {};
        return Frame { buffer, size };
    }

public:
    // A popup's view continues a traversable the engine already created for it. create_child and adopt_child_web_view are
    // protected on HeadlessWebView; this subclass is the sanctioned way in, the same one HeadlessWebView uses on itself.
    static PortWebView* create_child_for(PortWebView& parent, WebView::CanonicalTraversable& traversable)
    {
        // adopt_child_web_view takes ownership, and the engine relies on that for parent ownership, crash propagation and
        // child cleanup. Returning an owning pointer here would be a second owner of the same object, so the *parent*
        // keeps it and the caller only borrows.
        auto child = adopt_own(*new PortWebView(parent.m_theme, parent.m_viewport_size));
        auto* child_pointer = child.ptr();
        child->initialize_tab(Web::HTML::VisibilityState::Visible, traversable);
        parent.adopt_child_web_view(move(child));
        return child_pointer;
    }

private:
    // HeadlessWebView otherwise creates an internal child that the native host never learns about. UI-level requests
    // such as Command-click must fall through to PortApplication::open_blank_new_tab, which registers and reports it.
    virtual WebView::ViewImplementation* create_view_for_new_tab_or_window(WebView::IsPrivate) override { return nullptr; }

    PortWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize window_size)
        : HeadlessWebView(move(theme), window_size)
    {
    }

};

struct Tab {
    AK_ALLOC_WITH_KMALLOC;

    uint64_t id { 0 };
    // Owned here unless `borrowed`, in which case the parent view owns it (a popup) and this tab only points at it.
    OwnPtr<PortWebView> own_view;
    PortWebView* borrowed_view { nullptr };
    bool borrowed { false };

    PortWebView* view()
    {
        return borrowed ? borrowed_view : own_view.ptr();
    }

    // The engine's input positions are in device pixels and the host speaks CSS pixels, so the ratio has to be kept
    // alongside the tab. LbTabSetViewport is the only thing that sets it.
    double device_pixel_ratio { 1.0 };
    int32_t click_count { 0 };

    // before_unload is the one dialog the engine answers through a closure it hands us, not through a ViewImplementation
    // method, so the completion has to be stored until the host replies. Every other dialog is answered by calling into
    // the view, which keeps its own pending state.
    Function<void(bool)> pending_before_unload;

    // The picker requests the engine answers through a closure it hands us, for the same reason. The id is what a host
    // echoes back, so a stale answer for a request that was already superseded is ignored rather than misapplied.
    uint64_t pending_select_dropdown { 0 };
    uint64_t pending_file_picker { 0 };
    uint64_t pending_color_picker { 0 };
    struct PendingPermission {
        uint64_t request_id { 0 };
        Function<void(bool)> on_decided;
    };
    PendingPermission pending_permission;
    struct PendingExternalURL {
        uint64_t request_id { 0 };
        Function<void(bool)> on_complete;
    };
    PendingExternalURL pending_external_url;
};

}

struct LbApp {
    // OwnPtr, not NonnullOwnPtr: LbApp is allocated by plain new, so the kmalloc-tag static assert would fire otherwise.
    OwnPtr<PortApplication> application;
    Vector<NonnullOwnPtr<Tab>> tabs;
    LbCallbacks callbacks {};

    // The groups added after LbCallbacks. Separate structs rather than extra fields on LbCallbacks, because that
    // struct's layout is baked into a host's compiled-in offsets (see the header). Handlers read these at dispatch
    // time, so registering after the tabs exist works.
    LbUiCallbacks ui_callbacks {};
    LbPageCallbacks page_callbacks {};
    LbPickerCallbacks picker_callbacks {};

    // Ids are never reused: a popup's id must not collide with a tab the host is still holding a surface for.
    uint64_t next_tab_id { 1 };
    uint64_t active_tab_id { 0 };

    // One counter for every request group, so a stale answer is detectable by id alone.
    uint64_t next_request_id { 0 };

    // Chosen at tab creation because HeadlessWebView::system_theme() hands out the buffer its constructor was given.
    bool dark_theme { false };

    CFRunLoopTimerRef pump_timer { nullptr };

    // Tabs are declared AFTER the application, so they are destroyed BEFORE it: each tab's HeadlessWebView has to go
    // away while the application owning its WebContent client is still alive.
};

namespace {

void set_up_isolation(ByteString& scratch_directory)
{
    scratch_directory = ByteString::formatted("{}/Ladybird-Port-{}", Core::StandardPaths::tempfile_directory(), getpid());
    MUST(Core::Directory::create(scratch_directory, Core::Directory::CreateDirectories::Yes));
    VERIFY(setenv("XDG_CONFIG_HOME", scratch_directory.characters(), 1) == 0);
}

String encode_select_item(Web::HTML::SelectItem const& item)
{
    return item.visit(
        [](Web::HTML::SelectItemOption const& option) -> String {
            auto label = option.label.to_byte_string();
            auto value = option.value.to_byte_string();
            JsonObject object;
            object.set("type"sv, "option"sv);
            object.set("id"sv, static_cast<i64>(option.id));
            object.set("label"sv, label.view());
            object.set("value"sv, value.view());
            object.set("selected"sv, option.selected);
            object.set("enabled"sv, !option.disabled);
            return object.serialized();
        },
        [](Web::HTML::SelectItemOptionGroup const& group) -> String {
            JsonArray items;
            for (auto const& child : group.items) {
                JsonObject option;
                option.set("type"sv, "option"sv);
                option.set("id"sv, static_cast<i64>(child.id));
                auto child_label = child.label.to_byte_string();
                option.set("label"sv, child_label.view());
                auto child_value = child.value.to_byte_string();
                option.set("value"sv, child_value.view());
                option.set("selected"sv, child.selected);
                option.set("enabled"sv, !child.disabled);
                items.must_append(move(option));
            }
            JsonObject object;
            object.set("type"sv, "group"sv);
            auto group_label = group.label.to_byte_string();
            object.set("label"sv, group_label.view());
            object.set("items"sv, move(items));
            return object.serialized();
        },
        [](Web::HTML::SelectItemSeparator const&) -> String {
            JsonObject object;
            object.set("type"sv, "separator"sv);
            return object.serialized();
        });
}

String encode_select_items(Vector<Web::HTML::SelectItem> const& items)
{
    // Hand-assembled for the same reason as above: the array's elements are already-serialized JSON text, and
    // JsonArray::must_append(JsonValue) would re-encode them into a nested-string shape.
    StringBuilder builder;
    builder.append('[');
    for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0)
            builder.append(',');
        builder.append(encode_select_item(items[i]));
    }
    builder.append(']');
    return MUST(builder.to_string());
}

String encode_file_filters(Web::HTML::FileFilter const& filter)
{
    StringBuilder builder;
    builder.append('[');
    bool first = true;
    for (auto const& entry : filter.filters) {
        if (!first)
            builder.append(',');
        first = false;
        entry.visit(
            [&](Web::HTML::FileFilter::FileType type) {
                auto name = [&] {
                    switch (type) {
                    case Web::HTML::FileFilter::FileType::Audio:
                        return "audio"sv;
                    case Web::HTML::FileFilter::FileType::Image:
                        return "image"sv;
                    case Web::HTML::FileFilter::FileType::Video:
                        return "video"sv;
                    }
                    return "image"sv;
                }();
                JsonObject object;
                object.set("type"sv, name);
                builder.append(object.serialized());
            },
            [&](Web::HTML::FileFilter::MimeType const& mime) {
                JsonObject object;
                object.set("type"sv, "mime"sv);
                auto mime_value = mime.value.to_byte_string();
                object.set("value"sv, mime_value.view());
                builder.append(object.serialized());
            },
            [&](Web::HTML::FileFilter::Extension const& extension) {
                JsonObject object;
                object.set("type"sv, "extension"sv);
                auto extension_value = extension.value.to_byte_string();
                object.set("value"sv, extension_value.view());
                builder.append(object.serialized());
            });
    }
    builder.append(']');
    return MUST(builder.to_string());
}

// Utf16String -> the ABI's char*/len. Narrowed here because the header speaks bytes and the engine speaks UTF-16;
// `to_byte_string()` is the only correct narrowing and getting it wrong compiles either way (see Traps.md).
void dispatch_dialog_text(void (*callback)(void*, uint64_t, char const*, size_t), void* context, uint64_t tab, Utf16String const& text)
{
    if (!callback)
        return;
    auto narrow = text.to_byte_string();
    callback(context, tab, narrow.characters(), narrow.length());
}

void dispatch_paint(LbApp& app, uint64_t tab)
{
    if (app.callbacks.on_ready_to_paint)
        app.callbacks.on_ready_to_paint(app.callbacks.context, tab);
}

void dispatch_url(LbApp& app, uint64_t tab, auto const& url, void (*callback)(void*, uint64_t, char const*, size_t))
{
    if (!callback)
        return;
    // The bytes are valid for this call only, which the header states. URL::serialize() is an AK::String (UTF-16), so
    // it has to be narrowed before it can cross a char* boundary.
    auto serialized = url.serialize().to_byte_string();
    callback(app.callbacks.context, tab, serialized.characters(), serialized.length());
}

}

namespace {

// Shared lookup: every input entry point needs the same tab, and every one of them must tolerate a stale id.
Tab* find_tab(LbApp* app, uint64_t tab)
{
    if (!app)
        return nullptr;
    for (auto const& candidate : app->tabs) {
        if (candidate->id == tab)
            return candidate.ptr();
    }
    return nullptr;
}

void dispatch_title(LbApp& app, uint64_t tab, Utf16String const& title)
{
    if (!app.callbacks.on_title_change)
        return;
    auto narrow = title.to_byte_string();
    app.callbacks.on_title_change(app.callbacks.context, tab, narrow.characters(), narrow.length());
}

void dispatch_loading_state(LbApp& app, uint64_t tab, bool is_loading)
{
    if (!app.callbacks.on_loading_state_change)
        return;
    app.callbacks.on_loading_state_change(app.callbacks.context, tab, is_loading ? 1 : 0);
}

uint64_t register_existing_view(LbApp& app, PortWebView* view);

void dispatch_favicon(LbApp& app, uint64_t tab, Optional<Gfx::Bitmap const&> bitmap)
{
    if (!app.callbacks.on_favicon_change)
        return;

    auto* target = find_tab(&app, tab);
    if (!target)
        return;

    if (!bitmap.has_value()) {
        app.callbacks.on_favicon_change(app.callbacks.context, tab, nullptr, 0, nullptr, 0);
        return;
    }

    auto* view = target->view();
    auto const& hash = view->favicon_hash();
    ByteBuffer png_bytes;
    if (view->session().favicon_store && hash.has_value()) {
        if (auto stored_png = view->session().favicon_store->favicon_png(*hash); stored_png.has_value())
            png_bytes = stored_png.release_value();
    }

    if (png_bytes.is_empty()) {
        if (auto encoded = Gfx::PNGWriter::encode(*bitmap); !encoded.is_error())
            png_bytes = encoded.release_value();
    }

    if (png_bytes.is_empty()) {
        app.callbacks.on_favicon_change(app.callbacks.context, tab, nullptr, 0, nullptr, 0);
        return;
    }

    ByteString narrow_hash;
    if (hash.has_value())
        narrow_hash = hash->to_byte_string();

    app.callbacks.on_favicon_change(
        app.callbacks.context, tab,
        png_bytes.data(), png_bytes.size(),
        narrow_hash.characters(), narrow_hash.length()
    );
}

// Everything the groups added after LbCallbacks observe, installed per view. Split out of LbTabCreate because a popup's
// view is created by the engine, not by LbTabCreate, and must be wired identically.
// The handlers every view needs, whether the host asked for it or the page opened it. Split out of LbTabCreate because
// a popup's view is created by the engine: wiring only the newer groups there left popups without on_ready_to_paint (so
// the host could not present them) and without the dialog answers (so an alert() in a popup wedged it forever).
void install_core_callbacks(LbApp& app, uint64_t id, PortWebView& view)
{
    view.on_ready_to_paint = [&app, id] { dispatch_paint(app, id); };
    view.on_url_change = [&app, id](auto const& url) {
        dispatch_url(app, id, url, app.callbacks.on_url_change);
    };
    view.on_load_finish = [&app, id](auto const& url) {
        dispatch_url(app, id, url, app.callbacks.on_load_finish);
    };
    view.on_title_change = [&app, id](Utf16String const& title) {
        dispatch_title(app, id, title);
    };
    view.on_loading_state_change = [&app, id](bool is_loading) {
        dispatch_loading_state(app, id, is_loading);
    };
    view.on_favicon_change = [&app, id](Optional<Gfx::Bitmap const&> bitmap) {
        dispatch_favicon(app, id, bitmap);
    };

    // The four blocking dialogs, ALWAYS installed, whether or not the host supplied a callback.
    //
    // This used to be guarded by "if (app.callbacks.on_request_alert) { ... }", on the header's claim that an absent
    // callback makes the engine answer itself. It does not: PortWebView derives from HeadlessWebView, whose constructor
    // already installed handlers that only record Web::PendingDialog, so leaving them in place parks the page in
    // spin_event_loop_until_dialog_closed forever (Traps.md, "An absent dialog callback does NOT make the engine answer
    // itself"). Answering immediately with the safe outcome is what the header always promised, so the host may install
    // any subset of the four.
    view.on_request_alert = [&app, id](Utf16String const& message) {
        if (!app.callbacks.on_request_alert) {
            if (auto* target = find_tab(&app, id))
                target->view()->alert_closed();
            return;
        }
        dispatch_dialog_text(app.callbacks.on_request_alert, app.callbacks.context, id, message);
    };
    view.on_request_confirm = [&app, id](Utf16String const& message) {
        if (!app.callbacks.on_request_confirm) {
            if (auto* target = find_tab(&app, id))
                target->view()->confirm_closed(false);
            return;
        }
        dispatch_dialog_text(app.callbacks.on_request_confirm, app.callbacks.context, id, message);
    };
    view.on_request_prompt = [&app, id](Utf16String const& message, Utf16String const& default_text) {
        if (!app.callbacks.on_request_prompt) {
            if (auto* target = find_tab(&app, id))
                target->view()->prompt_closed(OptionalNone {});
            return;
        }
        auto narrow_message = message.to_byte_string();
        auto narrow_default = default_text.to_byte_string();
        app.callbacks.on_request_prompt(
            app.callbacks.context, id,
            narrow_message.characters(), narrow_message.length(),
            narrow_default.characters(), narrow_default.length());
    };
    view.on_request_before_unload = [&app, id](String const& source, Function<void(bool)> on_complete) {
        // The closure is the engine's own completion; store it so LbTabAnswerBeforeUnload can invoke it. A second
        // request replaces the first, which is what the engine does too (one pending dialog per page).
        auto* target = find_tab(&app, id);
        if (!app.callbacks.on_request_before_unload) {
            // Allowing the navigation is the answer the header documents as safe.
            on_complete(true);
            return;
        }
        if (target)
            target->pending_before_unload = move(on_complete);
        else
            on_complete(true);
        auto narrow_source = source.to_byte_string();
        app.callbacks.on_request_before_unload(app.callbacks.context, id, narrow_source.characters(), narrow_source.length());
    };

}

void install_group_callbacks(LbApp& app, uint64_t id, PortWebView& view)
{
    view.on_cursor_change = [&app, id](Gfx::Cursor cursor) {
        auto* callback = app.ui_callbacks.on_cursor_change;
        if (!callback)
            return;
        cursor.visit(
            [&](Gfx::StandardCursor standard) {
                callback(app.callbacks.context, id, static_cast<int32_t>(standard), nullptr, 0, 0, 0);
            },
            [&](Gfx::ImageCursor const& image) {
                auto* bitmap = image.bitmap.bitmap();
                if (!bitmap) {
                    callback(app.callbacks.context, id, static_cast<int32_t>(Gfx::StandardCursor::Arrow), nullptr, 0, 0, 0);
                    return;
                }
                auto encoded = Gfx::PNGWriter::encode(*bitmap);
                if (encoded.is_error()) {
                    callback(app.callbacks.context, id, static_cast<int32_t>(Gfx::StandardCursor::Arrow), nullptr, 0, 0, 0);
                    return;
                }
                callback(app.callbacks.context, id, LbCursorImage, encoded.value().data(), encoded.value().size(),
                    static_cast<int32_t>(image.hotspot.x()), static_cast<int32_t>(image.hotspot.y()));
            });
    };

    view.on_link_hover = [&app, id](URL::URL const& url) {
        auto* callback = app.ui_callbacks.on_link_hover;
        if (!callback)
            return;
        auto serialized = url.serialize().to_byte_string();
        callback(app.callbacks.context, id, serialized.characters(), serialized.length());
    };

    view.on_link_unhover = [&app, id] {
        if (auto* callback = app.ui_callbacks.on_link_unhover)
            callback(app.callbacks.context, id);
    };

    view.on_request_tooltip_override = [&app, id](Gfx::IntPoint position, ByteString const& tooltip) {
        auto* callback = app.ui_callbacks.on_request_tooltip_override;
        if (!callback)
            return;
        callback(app.callbacks.context, id, static_cast<int32_t>(position.x()), static_cast<int32_t>(position.y()),
            tooltip.characters(), tooltip.length());
    };

    view.on_stop_tooltip_override = [&app, id] {
        if (auto* callback = app.ui_callbacks.on_stop_tooltip_override)
            callback(app.callbacks.context, id);
    };

    view.on_enter_tooltip_area = [&app, id](ByteString const& tooltip) {
        if (auto* callback = app.ui_callbacks.on_enter_tooltip_area)
            callback(app.callbacks.context, id, tooltip.characters(), tooltip.length());
    };

    view.on_leave_tooltip_area = [&app, id] {
        if (auto* callback = app.ui_callbacks.on_leave_tooltip_area)
            callback(app.callbacks.context, id);
    };

    view.on_load_start = [&app, id] {
        if (auto* callback = app.page_callbacks.on_load_start)
            callback(app.callbacks.context, id);
    };

    view.on_top_level_navigation_commit = [&app, id] {
        if (auto* callback = app.page_callbacks.on_top_level_navigation_commit)
            callback(app.callbacks.context, id);
    };

    view.on_browser_history_traversal_complete = [&app, id] {
        if (auto* callback = app.page_callbacks.on_history_traversal_complete)
            callback(app.callbacks.context, id);
    };

    view.on_web_content_crashed = [&app, id](WebView::ViewImplementation::WebContentCrashReason reason) {
        if (auto* callback = app.page_callbacks.on_web_content_crashed)
            callback(app.callbacks.context, id, static_cast<int32_t>(reason));
    };

    view.on_crash_overlay_state_change = [&app, id](bool active) {
        if (auto* callback = app.page_callbacks.on_crash_overlay_state_change)
            callback(app.callbacks.context, id, active ? 1 : 0);
    };

    view.on_find_in_page = [&app, id](size_t current_match_index, Optional<size_t> const& total_match_count) {
        auto* callback = app.page_callbacks.on_find_in_page;
        if (!callback)
            return;
        callback(app.callbacks.context, id, current_match_index,
            total_match_count.has_value() ? 1 : 0,
            total_match_count.value_or(0));
    };

    // A popup. The engine hands us a fresh traversable; the shim creates and registers the view, so the host only ever
    // sees an ordinary tab id and never a CanonicalTraversable&.
    view.on_new_web_view = [&app, id](Web::HTML::ActivateTab activate_tab, Web::HTML::WebViewHints hints, WebView::CanonicalTraversable& traversable) -> String {
        auto* parent = find_tab(&app, id);
        if (!parent)
            return String::formatted("port-tab-{}", id).release_value();

        // Owned by the parent's view, but listed as its own tab so the host can present it.
        auto new_id = register_existing_view(app, PortWebView::create_child_for(*parent->view(), traversable));
        if (auto* callback = app.page_callbacks.on_new_tab)
            callback(app.callbacks.context, id, new_id, activate_tab == Web::HTML::ActivateTab::Yes ? 1 : 0, hints.popup ? 1 : 0);
        return String::formatted("port-tab-{}", new_id).release_value();
    };

    view.on_close = [&app, id] {
        // This closure lives on the view, and unlinking destroys the Tab that owns the view. Deferring keeps the engine's
        // stack (which may still use the view) and this closure alive; capturing by value rather than by reference
        // because `app` outlives it and `id` is already a copy, but the *closure itself* does not.
        auto* app_ptr = &app;
        auto closed_id = id;
        Core::deferred_invoke([app_ptr, closed_id] {
            auto* callback = app_ptr->page_callbacks.on_tab_closed;
            // Unlink before reporting: the host must not then call LbTabDestroy on an id that no longer exists.
            app_ptr->tabs.remove_first_matching([&](auto const& entry) { return entry->id == closed_id; });
            if (callback)
                callback(app_ptr->callbacks.context, closed_id);
        });
    };

    // Pickers and prompts. Same rule as the dialogs: the shim always installs a handler, and answers unavailable or
    // cancelled when the host supplied no callback, so a page never waits on a picker nobody will show.
    view.on_request_select_dropdown = [&app, id](Gfx::IntPoint position, i32 minimum_width, Vector<Web::HTML::SelectItem> items) {
        auto* target = find_tab(&app, id);
        if (!target)
            return;
        auto* callback = app.picker_callbacks.on_request_select_dropdown;
        if (!callback) {
            target->view()->select_dropdown_closed(OptionalNone {});
            return;
        }
        auto json = encode_select_items(items);
        auto request_id = ++app.next_request_id;
        target->pending_select_dropdown = request_id;
        callback(app.callbacks.context, id, request_id, static_cast<int32_t>(position.x()), static_cast<int32_t>(position.y()),
            static_cast<int32_t>(minimum_width), json.to_byte_string().characters(), json.to_byte_string().length());
    };

    view.on_request_file_picker = [&app, id](Web::HTML::FileFilter const& filters, Web::HTML::AllowMultipleFiles allow_multiple) {
        auto* target = find_tab(&app, id);
        if (!target)
            return;
        auto* callback = app.picker_callbacks.on_request_file_picker;
        if (!callback) {
            target->view()->file_picker_closed({});
            return;
        }
        auto json = encode_file_filters(filters);
        auto request_id = ++app.next_request_id;
        target->pending_file_picker = request_id;
        callback(app.callbacks.context, id, request_id, json.to_byte_string().characters(), json.to_byte_string().length(),
            allow_multiple == Web::HTML::AllowMultipleFiles::Yes ? 1 : 0);
    };

    view.on_request_color_picker = [&app, id](Color color) {
        auto* target = find_tab(&app, id);
        if (!target)
            return;
        auto* callback = app.picker_callbacks.on_request_color_picker;
        if (!callback) {
            target->view()->color_picker_update(OptionalNone {}, Web::HTML::ColorPickerUpdateState::Closed);
            return;
        }
        auto request_id = ++app.next_request_id;
        target->pending_color_picker = request_id;
        callback(app.callbacks.context, id, request_id, color.red(), color.green(), color.blue(), color.alpha());
    };

    view.on_request_permission = [&app, id](Utf16String const& name, URL::Origin const& origin, Function<void(bool)> on_decided) {
        auto* target = find_tab(&app, id);
        if (!target) {
            on_decided(false);
            return;
        }
        auto* callback = app.picker_callbacks.on_request_permission;
        if (!callback) {
            on_decided(false);
            return;
        }
        auto narrow_name = name.to_byte_string();
        auto narrow_origin = origin.serialize().to_byte_string();
        auto request_id = ++app.next_request_id;
        target->pending_permission = { request_id, move(on_decided) };
        callback(app.callbacks.context, id, request_id,
            narrow_name.characters(), narrow_name.length(),
            narrow_origin.characters(), narrow_origin.length());
    };

    view.on_request_external_url_confirmation = [&app, id](URL::URL const& url, URL::Origin const& origin, WebView::ExternalURLHandler const&, Function<void(bool)> on_complete) {
        auto* target = find_tab(&app, id);
        if (!target) {
            on_complete(false);
            return;
        }
        auto* callback = app.picker_callbacks.on_request_external_url_confirmation;
        if (!callback) {
            on_complete(false);
            return;
        }
        auto narrow_url = url.serialize().to_byte_string();
        auto narrow_origin = origin.serialize().to_byte_string();
        auto request_id = ++app.next_request_id;
        target->pending_external_url = { request_id, move(on_complete) };
        callback(app.callbacks.context, id, request_id,
            narrow_url.characters(), narrow_url.length(),
            narrow_origin.characters(), narrow_origin.length());
    };
}

uint64_t create_owned_tab(LbApp& app)
{
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv,
        app.dark_theme ? "Dark.ini"sv : "Default.ini"sv);
    auto theme = Gfx::load_system_theme(theme_path.string());
    if (theme.is_error()) {
        warnln("create_owned_tab: unable to load the system theme: {}", theme.error());
        return 0;
    }

    auto view = PortWebView::create(theme.release_value(), { 1024, 768 });
    auto id = app.next_tab_id++;
    install_core_callbacks(app, id, *view);
    install_group_callbacks(app, id, *view);

    auto tab = adopt_own(*new Tab {});
    tab->id = id;
    tab->own_view = move(view);
    app.tabs.append(move(tab));
    return id;
}

Optional<WebView::ViewImplementation&> PortApplication::open_blank_new_tab(Web::HTML::ActivateTab activate_tab) const
{
    if (!m_port_app)
        return {};
    auto new_id = create_owned_tab(*m_port_app);
    auto* tab = find_tab(m_port_app, new_id);
    if (!tab)
        return {};
    if (auto* callback = m_port_app->page_callbacks.on_new_tab)
        callback(m_port_app->callbacks.context, m_port_app->active_tab_id, new_id,
            activate_tab == Web::HTML::ActivateTab::Yes ? 1 : 0, 0);
    return *tab->view();
}

// Registers a view the engine created for a page. Ownership is the caller's to hand over; this only learns the id and
// wires the callbacks, so a popup behaves exactly like a tab the host asked for.
uint64_t register_existing_view(LbApp& app, PortWebView* view)
{
    auto id = app.next_tab_id++;
    auto raw = view;
    auto tab = adopt_own(*new Tab {});
    tab->id = id;
    tab->borrowed = true;
    tab->borrowed_view = raw;
    app.tabs.append(move(tab));
    install_core_callbacks(app, id, *raw);
    install_group_callbacks(app, id, *raw);
    return id;
}

// ---------------------------------------------------------------------------------------------------------------
// The groups added after LbCallbacks.

// A registration group is refused rather than trusted when the caller's struct is shorter than this dylib's, or speaks
// another version. Returning 0 costs the host that feature; reading past the end of the caller's struct would be a
// crash that looks like an engine bug.
template<typename T>
bool accepts_group(T const* callbacks)
{
    return callbacks && callbacks->struct_size >= sizeof(T) && callbacks->version == LB_CALLBACKS_VERSION;
}

// A tab id for a view the engine created for the page (a popup), registered exactly like a real tab so the host can call
// LbTabGetSurface/LbTabSetViewport on it. Ids are never reused.
uint64_t register_existing_view(LbApp& app, PortWebView* view);

void install_core_callbacks(LbApp& app, uint64_t id, PortWebView& view);
void install_group_callbacks(LbApp& app, uint64_t id, PortWebView& view);

}

extern "C" {

LbApp* LbAppCreate(char const* argv0, LbCallbacks const* callbacks)
{
    auto scratch_directory = ByteString {};
    set_up_isolation(scratch_directory);

    // The engine parses arguments itself, so it is handed a minimal, well-formed argv that cannot select headless mode
    // or a profile of its own.
    auto program_name = argv0 ? ByteString { StringView { argv0, strlen(argv0) } } : ByteString { "Ladybird"sv };
    auto arguments = Vector<ByteString> {};
    arguments.append(move(program_name));
    auto argument_strings = Vector<StringView> {};
    for (auto const& argument : arguments)
        argument_strings.append(argument.view());

    auto main_arguments = Main::Arguments {
        .argc = static_cast<int>(argument_strings.size()),
        .argv = nullptr,
        .strings = argument_strings.span(),
    };

    // The engine finds its helper processes and its Resources directory relative to the application directory. Tests get
    // it from a compile definition; here the host passes it, defaulting to the CLI's crude self-location.
    auto binary_path = Core::Environment::get("LADYBIRD_BINARY_PATH"sv);
    auto application = binary_path.has_value()
        ? PortApplication::create(main_arguments, *binary_path)
        : PortApplication::create(main_arguments, OptionalNone {});
    if (application.is_error()) {
        warnln("LbAppCreate: unable to initialize the engine: {}", application.error());
        return nullptr;
    }

    auto app = new LbApp {};
    app->application = application.release_value();
    app->application->set_port_app(app);
    if (callbacks)
        app->callbacks = *callbacks;
    return app;
}

int LbAppSetUiCallbacks(LbApp* app, LbUiCallbacks const* callbacks)
{
    if (!app || !accepts_group(callbacks))
        return 0;
    app->ui_callbacks = *callbacks;
    return 1;
}

int LbAppSetPageCallbacks(LbApp* app, LbPageCallbacks const* callbacks)
{
    if (!app || !accepts_group(callbacks))
        return 0;
    app->page_callbacks = *callbacks;
    return 1;
}

int LbAppSetPickerCallbacks(LbApp* app, LbPickerCallbacks const* callbacks)
{
    if (!app || !accepts_group(callbacks))
        return 0;
    app->picker_callbacks = *callbacks;
    return 1;
}

void LbAppSetTheme(LbApp* app, int dark)
{
    if (!app)
        return;
    app->dark_theme = dark != 0;
}

void LbAppPump(LbApp* app)
{
    if (!app)
        return;
    // PollForEvents maps to poll(timeout: 0), so this returns immediately. WaitForEvents would block the host's loop.
    Core::EventLoop::current().pump(Core::EventLoop::WaitMode::PollForEvents);
}

static void pump_from_run_loop_timer(CFRunLoopTimerRef, void* info)
{
    LbAppPump(static_cast<LbApp*>(info));
}

void LbAppStartPump(LbApp* app, double interval_seconds)
{
    if (!app || app->pump_timer)
        return;

    CFRunLoopTimerContext context { 0, app, nullptr, nullptr, nullptr };
    auto* timer = CFRunLoopTimerCreate(
        kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent(),
        interval_seconds,
        0,
        0,
        pump_from_run_loop_timer,
        &context);
    if (!timer)
        return;

    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopCommonModes);
    app->pump_timer = timer;
}

void LbAppStopPump(LbApp* app)
{
    if (!app || !app->pump_timer)
        return;
    CFRunLoopRemoveTimer(CFRunLoopGetCurrent(), app->pump_timer, kCFRunLoopCommonModes);
    CFRelease(app->pump_timer);
    app->pump_timer = nullptr;
}

void LbAppDestroy(LbApp* app)
{
    if (!app)
        return;
    LbAppStopPump(app);
    delete app;
}

uint64_t LbTabCreate(LbApp* app)
{
    return app ? create_owned_tab(*app) : 0;
}

void LbAppSetActiveTab(LbApp* app, uint64_t tab_id)
{
    if (app && find_tab(app, tab_id))
        app->active_tab_id = tab_id;
}

void LbTabAnswerAlert(LbApp* app, uint64_t tab)
{
    auto* target = find_tab(app, tab);
    if (!target)
        return;
    target->view()->alert_closed();
}

void LbTabAnswerConfirm(LbApp* app, uint64_t tab, int accept)
{
    auto* target = find_tab(app, tab);
    if (!target)
        return;
    target->view()->confirm_closed(accept != 0);
}

void LbTabAnswerPrompt(LbApp* app, uint64_t tab, char const* response, size_t len)
{
    auto* target = find_tab(app, tab);
    if (!target)
        return;
    // NULL means Cancel. An empty string means "accepted with no text" — a different outcome the engine distinguishes,
    // so it is not folded into the null case.
    if (!response) {
        target->view()->prompt_closed(OptionalNone {});
        return;
    }
    auto text = Utf16String::from_utf8(StringView { response, len });
    target->view()->prompt_closed(text);
}

void LbTabAnswerBeforeUnload(LbApp* app, uint64_t tab, int accept)
{
    auto* target = find_tab(app, tab);
    if (!target)
        return;
    // Take the completion before invoking it: the engine may issue another request from inside the callback, and
    // leaving a stale copy here would answer that one with this outcome.
    auto completion = move(target->pending_before_unload);
    target->pending_before_unload = nullptr;
    if (completion)
        completion(accept != 0);
}

void LbTabAnswerSelectDropdown(LbApp* app, uint64_t tab, uint64_t request_id, int has_selection, uint32_t selected_item_id)
{
    auto* target = find_tab(app, tab);
    if (!target || target->pending_select_dropdown != request_id)
        return;
    target->pending_select_dropdown = 0;
    if (!has_selection) {
        target->view()->select_dropdown_closed(OptionalNone {});
        return;
    }
    target->view()->select_dropdown_closed(selected_item_id);
}

void LbTabAnswerFilePicker(LbApp* app, uint64_t tab, uint64_t request_id, char const* json_or_null, size_t len)
{
    auto* target = find_tab(app, tab);
    if (!target || target->pending_file_picker != request_id)
        return;
    target->pending_file_picker = 0;

    // NULL/empty is Cancel. The host sends an array of filesystem paths, which LibWebView turns into SelectedFiles so the
    // page receives real file contents rather than a path it cannot read.
    Vector<Web::HTML::SelectedFile> files;
    if (json_or_null && len > 0) {
        auto parsed = JsonValue::from_string(StringView { json_or_null, len });
        if (!parsed.is_error() && parsed.value().is_array()) {
            parsed.value().as_array().for_each([&](JsonValue const& entry) {
                if (!entry.is_string())
                    return;
                auto path = entry.as_string().to_byte_string();
                if (auto file = WebView::create_selected_file(path); file.is_error())
                    warnln("LbTabAnswerFilePicker: unable to open {}: {}", path, file.error());
                else
                    files.append(file.release_value());
            });
        }
    }
    target->view()->file_picker_closed(move(files));
}

void LbTabAnswerColorPicker(LbApp* app, uint64_t tab, uint64_t request_id, int has_color, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    auto* target = find_tab(app, tab);
    if (!target || target->pending_color_picker != request_id)
        return;
    target->pending_color_picker = 0;
    if (!has_color) {
        target->view()->color_picker_update(OptionalNone {}, Web::HTML::ColorPickerUpdateState::Closed);
        return;
    }
    target->view()->color_picker_update(Color(r, g, b, a), Web::HTML::ColorPickerUpdateState::Closed);
}

void LbTabAnswerPermission(LbApp* app, uint64_t tab, uint64_t request_id, int granted)
{
    auto* target = find_tab(app, tab);
    if (!target || target->pending_permission.request_id != request_id)
        return;
    auto completion = move(target->pending_permission.on_decided);
    target->pending_permission = {};
    if (completion)
        completion(granted != 0);
}

void LbTabAnswerExternalURL(LbApp* app, uint64_t tab, uint64_t request_id, int accepted)
{
    auto* target = find_tab(app, tab);
    if (!target || target->pending_external_url.request_id != request_id)
        return;
    auto completion = move(target->pending_external_url.on_complete);
    target->pending_external_url = {};
    if (completion)
        completion(accepted != 0);
}

void LbTabReload(LbApp* app, uint64_t tab, int bypass_cache)
{
    auto* target = find_tab(app, tab);
    if (!target)
        return;
    // NOTE: WebView::BypassCache was introduced upstream after the tree the cached local build tree uses. This is the
    // correct call for the pin this fork tracks; a local build against that older tree cannot compile it, so it is the
    // one line here that only the pinned engine (or CI) can verify.
    target->view()->reload(bypass_cache != 0 ? WebView::BypassCache::Yes : WebView::BypassCache::No);
}

void LbTabStopLoading(LbApp* app, uint64_t tab)
{
    if (auto* target = find_tab(app, tab))
        target->view()->stop_loading();
}

void LbTabSetZoom(LbApp* app, uint64_t tab, double zoom_level)
{
    if (auto* target = find_tab(app, tab))
        target->view()->set_zoom(zoom_level);
}

void LbTabZoomIn(LbApp* app, uint64_t tab)
{
    if (auto* target = find_tab(app, tab))
        target->view()->zoom_in();
}

void LbTabZoomOut(LbApp* app, uint64_t tab)
{
    if (auto* target = find_tab(app, tab))
        target->view()->zoom_out();
}

double LbTabZoomLevel(LbApp* app, uint64_t tab)
{
    auto* target = find_tab(app, tab);
    return target ? target->view()->zoom_level() : 1.0;
}

void LbTabSetColorScheme(LbApp* app, uint64_t tab, int32_t color_scheme)
{
    auto* target = find_tab(app, tab);
    if (!target)
        return;
    auto scheme = Web::CSS::PreferredColorScheme::Auto;
    switch (color_scheme) {
    case LbPreferredColorSchemeLight:
        scheme = Web::CSS::PreferredColorScheme::Light;
        break;
    case LbPreferredColorSchemeDark:
        scheme = Web::CSS::PreferredColorScheme::Dark;
        break;
    default:
        scheme = Web::CSS::PreferredColorScheme::Auto;
        break;
    }
    target->view()->set_preferred_color_scheme(scheme);
}

void LbTabSetPreferredMotion(LbApp* app, uint64_t tab, int32_t motion)
{
    auto* target = find_tab(app, tab);
    if (!target)
        return;
    auto value = Web::CSS::PreferredMotion::Auto;
    switch (motion) {
    case LbPreferredMotionNoPreference:
        value = Web::CSS::PreferredMotion::NoPreference;
        break;
    case LbPreferredMotionReduce:
        value = Web::CSS::PreferredMotion::Reduce;
        break;
    default:
        value = Web::CSS::PreferredMotion::Auto;
        break;
    }
    target->view()->set_preferred_motion(value);
}

void LbTabSetPreferredContrast(LbApp* app, uint64_t tab, int32_t contrast)
{
    auto* target = find_tab(app, tab);
    if (!target)
        return;
    auto value = Web::CSS::PreferredContrast::Auto;
    switch (contrast) {
    case LbPreferredContrastLess:
        value = Web::CSS::PreferredContrast::Less;
        break;
    case LbPreferredContrastMore:
        value = Web::CSS::PreferredContrast::More;
        break;
    case LbPreferredContrastNoPreference:
        value = Web::CSS::PreferredContrast::NoPreference;
        break;
    default:
        value = Web::CSS::PreferredContrast::Auto;
        break;
    }
    target->view()->set_preferred_contrast(value);
}

void LbTabSetSystemVisibility(LbApp* app, uint64_t tab, int visible)
{
    auto* target = find_tab(app, tab);
    if (!target)
        return;
    target->view()->set_system_visibility_state(visible ? Web::HTML::VisibilityState::Visible : Web::HTML::VisibilityState::Hidden);
}

void LbTabSetSystemFocus(LbApp* app, uint64_t tab, int focused)
{
    if (auto* target = find_tab(app, tab))
        target->view()->set_has_system_focus(focused != 0);
}

void LbTabToggleMute(LbApp* app, uint64_t tab)
{
    if (auto* target = find_tab(app, tab))
        target->view()->toggle_page_mute_state();
}

int LbTabPageMuteState(LbApp* app, uint64_t tab)
{
    auto* target = find_tab(app, tab);
    if (!target)
        return 0;
    return target->view()->page_mute_state() == Web::HTML::MuteState::Muted ? 1 : 0;
}

void LbTabSelectAll(LbApp* app, uint64_t tab)
{
    if (auto* target = find_tab(app, tab))
        target->view()->select_all();
}

void LbTabUndo(LbApp* app, uint64_t tab)
{
    if (auto* target = find_tab(app, tab))
        target->view()->undo();
}

void LbTabRedo(LbApp* app, uint64_t tab)
{
    if (auto* target = find_tab(app, tab))
        target->view()->redo();
}

void LbTabFindInPage(LbApp* app, uint64_t tab, char const* utf8, size_t len, int match_case, int highlight_all)
{
    auto* target = find_tab(app, tab);
    if (!target || !utf8)
        return;
    target->view()->find_in_page(
        Utf16String::from_utf8(StringView { utf8, len }),
        match_case ? CaseSensitivity::CaseSensitive : CaseSensitivity::CaseInsensitive,
        highlight_all != 0);
}

void LbTabFindNextMatch(LbApp* app, uint64_t tab)
{
    if (auto* target = find_tab(app, tab))
        target->view()->find_in_page_next_match();
}

void LbTabFindPreviousMatch(LbApp* app, uint64_t tab)
{
    if (auto* target = find_tab(app, tab))
        target->view()->find_in_page_previous_match();
}

void LbTabFindEnd(LbApp* app, uint64_t tab)
{
    if (auto* target = find_tab(app, tab))
        target->view()->find_in_page_end();
}

void LbTabDestroy(LbApp* app, uint64_t tab)
{
    if (!app)
        return;
    // Unlinking from the vector is what stops callbacks and lookups from finding it; the tab's own view is destroyed by
    // the OwnPtr going away, before the application that owns its WebContent client does.
    for (auto& candidate : app->tabs) {
        if (candidate->id != tab)
            continue;
        app->tabs.remove_first_matching([&](auto const& entry) { return entry.ptr() == candidate.ptr(); });
        return;
    }
}

void LbTabLoad(LbApp* app, uint64_t tab, char const* url, size_t len)
{
    if (!app || !url)
        return;
    for (auto const& candidate : app->tabs) {
        if (candidate->id != tab)
            continue;
        auto parsed = URL::Parser::basic_parse(StringView { url, len });
        if (!parsed.has_value()) {
            warnln("LbTabLoad: unable to parse the URL");
            return;
        }
        candidate->view()->load(parsed.release_value());
        return;
    }
}

int LbResolveUserInput(char const* utf8, size_t len, char** out_url, size_t* out_len)
{
    if (!utf8 || !out_url || !out_len)
        return 0;
    *out_url = nullptr;
    *out_len = 0;

    // The settings engine is only populated from Settings.json, which this host has no UI to write, and which a fresh
    // profile does not contain at all -- so a new user would type a search and get nothing. Falling back to the first
    // builtin keeps search working out of the box; once a settings UI exists, the configured engine wins and this
    // branch stops being taken.
    Optional<WebView::SearchEngine> search_engine;
    if (WebView::Application::is_initialized()) {
        search_engine = WebView::Application::settings().search_engine_settings().engine;
    }
    if (!search_engine.has_value()) {
        if (auto builtins = WebView::builtin_search_engines(); !builtins.is_empty())
            search_engine = builtins[0];
    }

    auto url = WebView::sanitize_url(StringView { utf8, len }, search_engine);
    if (!url.has_value())
        return 0;

    auto serialized = url->serialize();
    auto bytes = serialized.bytes();
    // malloc, not the engine's allocator: the host is a different toolchain and frees with LbStringFree, which must not
    // reach into an allocator it does not share.
    auto* buffer = static_cast<char*>(malloc(bytes.size()));
    if (!buffer)
        return 0;
    memcpy(buffer, bytes.data(), bytes.size());

    *out_url = buffer;
    *out_len = bytes.size();
    return 1;
}

void LbStringFree(char* string)
{
    free(string);
}

int LbTabGetSurface(LbApp* app, uint64_t tab, LbSurface* out)
{
    if (!app || !out)
        return 0;
    for (auto const& candidate : app->tabs) {
        if (candidate->id != tab)
            continue;

        auto frame = candidate->view()->current_frame();
        if (!frame.has_value())
            return 0;

        auto const& handle = frame->buffer->iosurface_handle();
        if (frame->painted_size.is_empty())
            return 0;

        // R1: hand the reference across; never crop or clone the bitmap behind it. The ref is borrowed, so the caller
        // retains it if it needs the surface past the next paint.
        out->iosurface = handle.core_foundation_pointer();
        out->painted_width = frame->painted_size.width();
        out->painted_height = frame->painted_size.height();
        out->surface_width = static_cast<int32_t>(handle.width());
        out->surface_height = static_cast<int32_t>(handle.height());
        return 1;
    }
    return 0;
}

void LbTabSetViewport(LbApp* app, uint64_t tab, int32_t width, int32_t height, double device_pixel_ratio)
{
    if (!app || width <= 0 || height <= 0)
        return;
    if (device_pixel_ratio <= 0)
        device_pixel_ratio = 1.0;

    for (auto const& candidate : app->tabs) {
        if (candidate->id != tab)
            continue;

        // The ratio has to be in place before the resize, because the resize is what pushes the viewport. This is the
        // only public way to set it without touching Libraries/; a real port wants a non-testing setter here.
        candidate->view()->set_device_pixel_ratio_for_testing(candidate->view()->page(), device_pixel_ratio);
        // Kept for input: positions arrive in CSS pixels and the engine wants device pixels.
        candidate->device_pixel_ratio = device_pixel_ratio;

        // A headless viewport is in device pixels, the same way Qt's update_viewport_size() scales by the ratio.
        candidate->view()->reset_viewport_size({
            static_cast<double>(width) * device_pixel_ratio,
            static_cast<double>(height) * device_pixel_ratio,
        });
        return;
    }
}

void LbTabTraverseHistory(LbApp* app, uint64_t tab_id, int32_t delta)
{
    auto* tab = find_tab(app, tab_id);
    if (!tab)
        return;

    // The ABI is the two buttons a browser has, so anything else is one of them rather than an arbitrary jump.
    auto direction = delta < 0 ? -1 : 1;
    tab->view()->traverse_the_history_by_delta(direction);
}

int LbTabCanTraverseHistory(LbApp* app, uint64_t tab_id, int32_t delta)
{
    auto* tab = find_tab(app, tab_id);
    if (!tab)
        return 0;

    // The engine's actions already encode whether there is anywhere to go, and LibWebView keeps them current as
    // navigation happens. Deriving this from history step counts instead would duplicate a rule that can drift.
    auto& action = delta < 0 ? tab->view()->navigate_back_action() : tab->view()->navigate_forward_action();
    return action.enabled() ? 1 : 0;
}

void LbTabSendMouseWithModifiers(LbApp* app, uint64_t tab_id, LbMouseEventType type, double x, double y, LbMouseButton button, LbMouseButton buttons, int32_t click_count, double wheel_delta_y, uint32_t modifiers)
{
    auto* tab = find_tab(app, tab_id);
    if (!tab)
        return;

    // CSS pixels in, device pixels out: the same conversion LbTabSetViewport applies to the viewport, so a click lands
    // where the user sees the cursor regardless of the display's scale.
    auto const ratio = tab->device_pixel_ratio;
    Web::DevicePixelPoint position {
        static_cast<double>(x) * ratio,
        static_cast<double>(y) * ratio,
    };

    Web::MouseEvent event {
        .type = [&] {
            switch (type) {
            case LbMouseEventDown:
                return Web::MouseEvent::Type::MouseDown;
            case LbMouseEventUp:
                return Web::MouseEvent::Type::MouseUp;
            case LbMouseEventMove:
                return Web::MouseEvent::Type::MouseMove;
            case LbMouseEventWheel:
                return Web::MouseEvent::Type::MouseWheel;
            }
            return Web::MouseEvent::Type::MouseMove;
        }(),
        .position = position,
        // The engine uses this only for synthesizing events it delivers itself, so mirroring position is correct here.
        .screen_position = position,
        .button = static_cast<Web::UIEvents::MouseButton>(button),
        .buttons = static_cast<Web::UIEvents::MouseButton>(buttons),
        .modifiers = static_cast<Web::UIEvents::KeyModifier>(modifiers),
        .wheel_delta_y = wheel_delta_y,
        .browser_data = nullptr,
    };

    if (type == LbMouseEventWheel) {
        // There is no WheelEvent: a wheel is a MouseEvent, and the tick fields are derived exactly the way the platform
        // backends derive them. Leaving them zero makes the compositor treat the scroll as something other than precise.
        event.wheel_tick_120ths_y = Web::wheel_tick_120ths_from_pixel_delta(wheel_delta_y, Web::WheelDeltaPrecision::Precise);
        event.wheel_delta_precision = Web::WheelDeltaPrecision::Precise;
    } else {
        event.click_count = click_count;
        // A down/up pair with no movement is what the engine reads as a click; the second click of a double-click
        // reuses click_count = 2, which the host counts because the engine does not.
        if (type == LbMouseEventDown)
            tab->click_count = click_count;
    }

    tab->view()->enqueue_input_event(move(event));
}

void LbTabSendMouse(LbApp* app, uint64_t tab_id, LbMouseEventType type, double x, double y, LbMouseButton button, LbMouseButton buttons, int32_t click_count, double wheel_delta_y)
{
    LbTabSendMouseWithModifiers(app, tab_id, type, x, y, button, buttons, click_count, wheel_delta_y, 0);
}

void LbTabSendKey(LbApp* app, uint64_t tab_id, int32_t key, uint32_t code_point, uint32_t modifiers, int is_key_down)
{
    auto* tab = find_tab(app, tab_id);
    if (!tab)
        return;

    auto type = is_key_down ? Web::KeyEvent::Type::KeyDown : Web::KeyEvent::Type::KeyUp;
    Web::KeyEvent event {
        .type = type,
        .key = static_cast<Web::UIEvents::KeyCode>(key),
        .modifiers = static_cast<Web::UIEvents::KeyModifier>(modifiers),
        .code_point = code_point,
        .repeat = false,
        // The one field a host would never set by accident and cannot omit without a silent no-op: without it the key is
        // delivered, handled, and inserts nothing. Text goes in on keydown, never on keyup.
        .should_insert_text = is_key_down && code_point != 0,
        .browser_data = nullptr,
    };

    tab->view()->enqueue_input_event(move(event));
}

void LbTabTypeText(LbApp* app, uint64_t tab_id, char const* utf8, size_t len)
{
    auto* tab = find_tab(app, tab_id);
    if (!tab || !utf8)
        return;

    // Decode UTF-8 to code points here rather than in the host: the ABI is C, and a Swift caller passing a Swift String
    // is handing over exactly these bytes anyway.
    auto view = Utf8View { StringView { utf8, len } };
    for (auto cp : view) {
        Web::KeyEvent event {
            .type = Web::KeyEvent::Type::KeyDown,
            .key = Web::UIEvents::KeyCode::Key_Invalid,
            .modifiers = Web::UIEvents::KeyModifier::Mod_None,
            .code_point = cp,
            .repeat = false,
            .should_insert_text = true,
            .browser_data = nullptr,
        };
        tab->view()->enqueue_input_event(move(event));
    }
}

}
