/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>

#include "../../Libraries/LibPort/ladybird_port.h"
#include <stdlib.h>

namespace {

class TestApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode.clear();
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

// There is no WheelEvent: a wheel is a MouseEvent with Type::MouseWheel. The tick fields are derived from the pixel
// delta, exactly as the platform backends do it.
void scroll_wheel_at(WebView::ViewImplementation& view, Web::DevicePixelPoint position, double wheel_delta_y)
{
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseWheel,
        .position = position,
        .screen_position = position,
        .wheel_delta_y = wheel_delta_y,
        .wheel_tick_120ths_y = Web::wheel_tick_120ths_from_pixel_delta(wheel_delta_y, Web::WheelDeltaPrecision::Precise),
        .wheel_delta_precision = Web::WheelDeltaPrecision::Precise,
        .browser_data = nullptr,
    });
}

void click_at(WebView::ViewImplementation& view, Web::DevicePixelPoint position, Web::UIEvents::KeyModifier modifiers = Web::UIEvents::Mod_None)
{
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseDown,
        .position = position,
        .screen_position = position,
        .button = Web::UIEvents::MouseButton::Primary,
        .buttons = Web::UIEvents::MouseButton::Primary,
        .modifiers = modifiers,
        .click_count = 1,
        .browser_data = nullptr,
    });
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseUp,
        .position = position,
        .screen_position = position,
        .button = Web::UIEvents::MouseButton::Primary,
        .modifiers = modifiers,
        .click_count = 1,
        .browser_data = nullptr,
    });
}

// A text field is the only way to observe this: the engine reports no callback for "a character was inserted", so the
// assertion has to read page state, exactly like the wheel case.
void press_key(WebView::ViewImplementation& view, Web::UIEvents::KeyCode key, u32 code_point, Web::UIEvents::KeyModifier modifiers = Web::UIEvents::Mod_None)
{
    // `should_insert_text` is what actually inserts the character. Without it the event is delivered, the key is
    // handled, and nothing appears: a silent no-op. Platforms set it to "KeyDown and we have text".
    for (auto type : { Web::KeyEvent::Type::KeyDown, Web::KeyEvent::Type::KeyUp }) {
        view.enqueue_input_event(Web::KeyEvent {
            .type = type,
            .key = key,
            .modifiers = modifiers,
            .code_point = code_point,
            .repeat = false,
            .should_insert_text = type == Web::KeyEvent::Type::KeyDown,
            .browser_data = nullptr,
        });
    }
}

// A blocked spin would hang the probe, so every wait is bounded.
bool spin_until_or_timeout(Function<bool()> condition, AK::Duration timeout)
{
    auto deadline = MonotonicTime::now() + timeout;
    while (!condition()) {
        if (MonotonicTime::now() > deadline)
            return false;
        Core::EventLoop::current().pump();
    }
    return true;
}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_directory = ByteString::formatted("{}/Ladybird-TestNativeInput-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_directory, Core::Directory::CreateDirectories::Yes));
    ScopeGuard cleanup = [&] { MUST(FileSystem::remove(test_directory, FileSystem::RecursionMode::Allowed)); };
    VERIFY(setenv("XDG_CONFIG_HOME", test_directory.characters(), 1) == 0);

#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif

    // Address-bar classification, before any page exists. This is the seam the host used to fake by prefixing
    // "https://", so the cases that matter are the ones that prefixing got wrong: prose became a bogus host, and a
    // bare host with a port parsed as a scheme.
    {
        struct Case {
            StringView input;
            bool expect_url;
            StringView expect_contains;
        };
        // XDG_CONFIG_HOME points at a fresh temp directory, so no Settings.json exists and no search engine is
        // configured. That is exactly the first-run state a new user is in, and the shim must still search -- the
        // fallback to a builtin engine is what makes that true, and this is the test that pins it.
        Case const cases[] = {
            { "https://example.com/path?q=1"sv, true, "https://example.com/path?q=1"sv },
            { "example.com"sv, true, "example.com"sv },
            { "localhost:8080"sv, true, ":8080"sv },
            // Prose must become a search URL, not a bogus host. The scheme is the assertion that matters: a query goes
            // to the engine's search endpoint, and "https://ladybird browser/" would be a navigation to garbage.
            { "ladybird browser"sv, true, "?q="sv },
            { "hello world"sv, true, "hello%20world"sv },
        };

        for (auto const& test : cases) {
            char* resolved = nullptr;
            size_t resolved_len = 0;
            auto ok = LbResolveUserInput(test.input.characters_without_null_termination(), test.input.length(), &resolved, &resolved_len);
            StringView got = resolved ? StringView { resolved, resolved_len } : StringView {};
            outln("RESOLVE: '{}' -> {}", test.input, got);

            if (ok != (test.expect_url ? 1 : 0)) {
                outln("FAIL: '{}' resolved={} expected={}", test.input, ok, test.expect_url ? 1 : 0);
                LbStringFree(resolved);
                return 1;
            }
            if (test.expect_url && !got.contains(test.expect_contains)) {
                outln("FAIL: '{}' resolved to '{}', expected it to contain '{}'", test.input, got, test.expect_contains);
                LbStringFree(resolved);
                return 1;
            }
            LbStringFree(resolved);
        }
        outln("PASS: address-bar input is classified by the engine");
    }

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));
    auto view = WebView::HeadlessWebView::create(theme, { 1200, 800 });

    size_t loads_finished = 0;
    size_t paints_since_keyboard_page = 0;
    Optional<URL::URL> last_loaded_url;
    Utf16String last_title;

    view->on_load_finish = [&](auto const& url) {
        outln("LOADED #{}: {}", loads_finished + 1, url);
        ++loads_finished;
        last_loaded_url = url;
    };
    view->on_title_change = [&](auto const& title) { last_title = title; };
    // A paint is what builds an <input>'s shadow tree, so the keyboard phase has to wait for one.
    view->on_ready_to_paint = [&] { ++paints_since_keyboard_page; };

    // Reads a value out of the page. The alert round-trip is the pattern TestCrashOverlay uses.
    auto evaluate = [&](StringView source) -> Optional<JsonValue> {
        Optional<JsonValue> result;
        view->on_request_alert = [&](Utf16String const& value) {
            result = MUST(JsonValue::from_string(value.to_utf8()));
            view->alert_closed();
        };
        view->run_javascript(MUST(String::formatted("alert(JSON.stringify(({}) ?? null))", source)));
        auto responded = spin_until_or_timeout([&] { return result.has_value(); }, AK::Duration::from_seconds(15));
        view->on_request_alert = nullptr;
        if (!responded)
            return {};
        return result.release_value();
    };

    auto const wheel_position = Web::DevicePixelPoint { 600, 400 };

    // pump() defaults to WaitForEvents, which blocks in poll() until a timer expires or an IPC message arrives. With no
    // timer pending that is forever, and the timeout checks below would never run. A heartbeat timer bounds the wait.
    int heartbeats = 0;
    auto heartbeat = Core::Timer::create_repeating(AK::Duration::from_milliseconds(20), [&] { ++heartbeats; });
    heartbeat->start();
    ScopeGuard stop_heartbeat = [&] { heartbeat->stop(); };

    // Navigating before the initial about:blank load completes drops the navigation.
    if (!spin_until_or_timeout([&] { return loads_finished >= 1; }, AK::Duration::from_seconds(30))) {
        outln("FAIL: the initial load never finished");
        return 1;
    }

    // A page three viewports tall that reports where it scrolled to through its title, so the probe can wait on the
    // scroll landing instead of on a fixed delay.
    view->load_html(R"~~~(<!DOCTYPE html>
<title>unscrolled</title>
<body style="margin:0;height:3000px">
<script>
addEventListener("scroll", () => { document.title = "scrolled to " + scrollY; });
</script>
</body>)~~~"sv);

    if (!spin_until_or_timeout([&] { return loads_finished >= 2; }, AK::Duration::from_seconds(30))) {
        outln("FAIL: the scroll page never finished loading");
        return 1;
    }
    VERIFY(last_title == "unscrolled"sv);

    auto scroll_y_before = evaluate("window.scrollY"sv);
    outln("SCROLLY BEFORE: {}", scroll_y_before.has_value() ? scroll_y_before->serialized() : "unreadable"_string);

    // POSITIVE delta_y means "scroll the content up", i.e. later content enters the viewport from the bottom and
    // scrollY INCREASES. This is the unit the UI must convert into: AppKit reports a scroll-down gesture as a NEGATIVE
    // deltaY under natural scrolling (content follows the fingers), so the ship must negate it. Recorded here so the
    // sign is ground truth from the engine, not inferred from a screenshot.
    scroll_wheel_at(*view, wheel_position, 120);

    // Wheel is routed through the compositor, which may scroll asynchronously and consume the event. Waiting on the
    // page's own report is the only way to tell a real scroll from a dropped one.
    auto scrolled = spin_until_or_timeout([&] { return last_title.starts_with("scrolled to "sv); }, AK::Duration::from_seconds(20));
    auto settled = spin_until_or_timeout([&] { return view->pending_input_event_count_for_testing() == 0; }, AK::Duration::from_seconds(20));
    outln("WHEEL: page_reported_scroll={} input_settled={} title={}", scrolled, settled, last_title);

    auto scroll_y_after = evaluate("window.scrollY"sv);
    auto scroll_y = scroll_y_after.has_value() ? scroll_y_after->get_double_with_precision_loss().value_or(-1) : -1;
    outln("SCROLLY AFTER: {}", scroll_y);

    if (!scrolled || scroll_y <= 0) {
        outln("FAIL: the wheel event did not scroll the page");
        return 1;
    }

    // State the sign explicitly: a positive wheel delta_y must increase scrollY (later content comes into view).
    outln("SIGN: wheel delta_y=+120 -> scrollY={} (positive delta scrolls the content up, scrollY increases)", scroll_y);

    // And the converse, so the conversion is pinned in both directions rather than by one sample.
    {
        auto before_negative = evaluate("window.scrollY"sv);
        auto before_value = before_negative.has_value() ? before_negative->get_double_with_precision_loss().value_or(0) : 0;
        last_title = {};
        scroll_wheel_at(*view, wheel_position, -60);
        auto backed = spin_until_or_timeout([&] { return last_title.starts_with("scrolled to "sv); }, AK::Duration::from_seconds(20));
        spin_until_or_timeout([&] { return view->pending_input_event_count_for_testing() == 0; }, AK::Duration::from_seconds(20));
        auto after_negative = evaluate("window.scrollY"sv);
        auto after_value = after_negative.has_value() ? after_negative->get_double_with_precision_loss().value_or(0) : 0;
        outln("SIGN: wheel delta_y=-60 -> scrollY {} -> {} (negative delta scrolls down)", before_value, after_value);
        if (!backed || after_value >= before_value) {
            outln("FAIL: a negative wheel delta did not decrease scrollY");
            return 1;
        }
    }

    // A link filling the viewport, so the click position need not be measured.
    view->load_html(R"~~~(<!DOCTYPE html>
<title>click-me</title>
<body style="margin:0">
<a href="https://example.com/" style="display:block;width:100vw;height:100vh">go</a>
</body>)~~~"sv);

    if (!spin_until_or_timeout([&] { return loads_finished >= 3; }, AK::Duration::from_seconds(30))) {
        outln("FAIL: the click page never finished loading");
        return 1;
    }

    outln("READY: loading a paint-free state, now clicking the link");
    auto loads_before_click = loads_finished;
    click_at(*view, wheel_position);

    auto navigated = spin_until_or_timeout([&] { return loads_finished > loads_before_click; }, AK::Duration::from_seconds(45));
    outln("CLICK: navigated={} loads={} url={}", navigated, loads_finished, last_loaded_url.has_value() ? last_loaded_url->serialize() : "none"_string);

    if (!navigated) {
        outln("FAIL: clicking the link did not navigate");
        return 1;
    }

    outln("PASS: wheel scrolls the page and a click on a link navigates");

    // A native host must preserve mouse modifiers; otherwise Command-click arrives as an ordinary click. Observe the
    // modifier in the page so this probe pins the input boundary without depending on a platform tab implementation.
    paints_since_keyboard_page = 0;
    auto loads_before_command_click = loads_finished;
    view->load_html(R"~~~(<!DOCTYPE html>
<title>command-click-me</title>
<body style="margin:0">
<a href="https://example.com/command-click" style="display:block;width:100vw;height:100vh">go</a>
<script>addEventListener("click", event => { event.preventDefault(); document.title = `command-click:${event.metaKey}`; });</script>
</body>)~~~"sv);

    if (!spin_until_or_timeout([&] { return loads_finished > loads_before_command_click; }, AK::Duration::from_seconds(30))) {
        outln("FAIL: the command-click page never finished loading");
        return 1;
    }
    if (!spin_until_or_timeout([&] { return paints_since_keyboard_page > 0; }, AK::Duration::from_seconds(30))) {
        outln("FAIL: the command-click page never painted");
        return 1;
    }
    click_at(*view, wheel_position, Web::UIEvents::Mod_Super);
    auto command_modifier_arrived = spin_until_or_timeout([&] { return last_title == "command-click:true"sv; }, AK::Duration::from_seconds(20));
    outln("COMMAND CLICK: modifier_arrived={} title={}", command_modifier_arrived, last_title);
    if (!command_modifier_arrived) {
        outln("FAIL: Command-click did not reach the page with metaKey=true");
        return 1;
    }
    outln("PASS: Command-click reaches the page with metaKey=true");

    // -----------------------------------------------------------------------------------------
    // Keyboard. The last engine input path that had never crossed this boundary.
    //
    // A text field is the observable: the engine reports no callback for "a character was inserted", so page state is
    // the only witness. Same lesson as wheel, which the compositor consumes without telling WebContent.
    // -----------------------------------------------------------------------------------------
    outln("");
    outln("PHASE 2: typing into a text field");

    auto loads_before_keyboard = loads_finished;
    paints_since_keyboard_page = 0;
    view->load(URL::Parser::basic_parse("data:text/html,%3Cbody%20style%3D%27margin%3A0%27%3E%3Cinput%20id%3Df%20style%3D%27width%3A300px%3Bheight%3A30px%27%3E%3Cscript%3Ewindow.log%3D%5B%5D%3BaddEventListener%28%27keydown%27%2Cfunction%28e%29%7Bwindow.log.push%28%27down%3A%27%2Be.key%29%7D%29%3BaddEventListener%28%27beforeinput%27%2Cfunction%28e%29%7Bwindow.log.push%28%27beforeinput%3A%27%2Be.data%29%7D%29%3C%2Fscript%3E"sv).release_value());

    if (!spin_until_or_timeout([&] { return loads_finished > loads_before_keyboard; }, AK::Duration::from_seconds(30))) {
        outln("FAIL: the text-field page never finished loading");
        return 1;
    }

    // The page MUST be painted before typing can be observed. An <input>'s text node lives in its shadow tree, which
    // is built during layout; an unpainted view never generates a box, so `handle_insert` returns at its first guard
    // (`!text_node`) and the character is silently dropped. The keystrokes still reach the DOM - keydown, keypress and
    // beforeinput all fire - which makes this look like an engine bug when it is a missing paint. Wait for a frame.
    if (!spin_until_or_timeout([&] { return paints_since_keyboard_page > 0; }, AK::Duration::from_seconds(30))) {
        outln("FAIL: the text-field page never painted, so typing cannot be observed");
        return 1;
    }
    outln("PAINTED: the field has a box, so it has a text node");

    // Focus through the DOM, not a click: a headless view has never had system focus, and a click that misses the
    // input produces a diagnostic about focus instead of about keyboards.
    click_at(*view, { 60, 12 });

    auto read_value = [&]() -> String {
        Optional<String> value;
        view->on_request_alert = [&](Utf16String const& text) {
            value = text.to_utf8();
            view->alert_closed();
        };
        view->run_javascript("alert(document.getElementById('f').value)"_string);
        spin_until_or_timeout([&] { return value.has_value(); }, AK::Duration::from_seconds(10));
        view->on_request_alert = nullptr;
        return value.value_or(""_string);
    };

    outln("VALUE BEFORE: '{}'", read_value());

    // Send the keys immediately, with nothing intervening: no JS-driven focus, no value assignment. Every diagnostic
    // step above this line was itself a JS evaluation, and a JS evaluation is a chance for the page to diverge from
    // what the ABI path sees.
    press_key(*view, Web::UIEvents::KeyCode::Key_H, 'h');
    press_key(*view, Web::UIEvents::KeyCode::Key_I, 'i');

    // A key event is async: UI -> Compositor -> WebContent -> page. Reading the value immediately reads before the
    // round trip finishes, which looks exactly like "the key never arrived". Wait for the queue to drain.
    spin_until_or_timeout([&] { return view->pending_input_event_count_for_testing() == 0; }, AK::Duration::from_seconds(10));
    // The queue draining only means the events were sent; give the page a few turns to run its default action.
    for (int turn = 0; turn < 20; ++turn)
        spin_until_or_timeout([] { return false; }, AK::Duration::from_milliseconds(50));

    auto typed = read_value();
    outln("VALUE AFTER:  '{}'", typed);


    if (typed != "hi"_string) {
        outln("FAIL: keyboard input did not reach the page (expected 'hi', got '{}')", typed);
        return 1;
    }

    outln("PASS: keyboard input reaches a focused text field");
    return 0;
}
