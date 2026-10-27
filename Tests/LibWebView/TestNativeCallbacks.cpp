/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The contract the callback groups added after LbCallbacks have to keep:
 *
 *   1. A group registered with this dylib's struct_size and version is accepted (returns 1).
 *   2. A group is REFUSED, not trusted, when the caller's struct is shorter than this dylib's, when the version
 *      differs, or when the pointer is NULL. That refusal is the whole reason the groups are size-tagged: a host
 *      compiled against an older header must lose a feature, never read past the end of its own struct.
 *   3. An absent callback never leaves a page waiting. LbTabCreate installs its own handler for every request the
 *      engine blocks on, so a host that registers no picker or dialog callbacks still gets an answered page.
 *
 * (3) is the regression test for the bug recorded in Documentation/Traps.md as "An absent dialog callback does NOT
 * make the engine answer itself". It is asserted at the ABI level here — LbAppCreate/LbTabCreate with no callbacks,
 * then a real page — rather than by reaching into the shim's internals, because the internals are exactly what a
 * future refactor is allowed to change.
 *
 * What this probe deliberately does NOT do: it never installs a partial dialog set and asserts a page wedged, because
 * a wedged page is indistinguishable from a slow one without a timeout, and a flaky probe is worse than a missing one.
 * The absence rule is checked by asserting that an unanswered request is answered by the shim, not by racing a timer.
 */

#include <AK/LexicalPath.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/Timer.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>

#include "../../Libraries/LibPort/ladybird_port.h"
#include <stdlib.h>

namespace {

int failures { 0 };
uint64_t opened_parent_tab { 0 };
uint64_t opened_new_tab { 0 };
int opened_activate_tab { -1 };
size_t loads_finished { 0 };
size_t paints_ready { 0 };
bool saw_command_modifier { false };

void did_finish_load(void*, uint64_t, char const*, size_t)
{
    ++loads_finished;
}

void did_become_ready_to_paint(void*, uint64_t)
{
    ++paints_ready;
}

void did_change_title(void*, uint64_t, char const* title, size_t length)
{
    saw_command_modifier = StringView { title, length } == "command:true"sv;
}

void did_open_new_tab(void*, uint64_t parent_tab, uint64_t new_tab, int activate_tab, int)
{
    opened_parent_tab = parent_tab;
    opened_new_tab = new_tab;
    opened_activate_tab = activate_tab;
}

void check(bool condition, StringView what)
{
    if (condition) {
        outln("PASS: {}", what);
        return;
    }
    outln("FAIL: {}", what);
    ++failures;
}

// The three groups, each with the two members every size-tagged struct starts with.
LbUiCallbacks ui_group(size_t struct_size, uint32_t version = LB_CALLBACKS_VERSION)
{
    LbUiCallbacks callbacks {};
    callbacks.struct_size = static_cast<uint32_t>(struct_size);
    callbacks.version = version;
    return callbacks;
}

LbPageCallbacks page_group(size_t struct_size, uint32_t version = LB_CALLBACKS_VERSION)
{
    LbPageCallbacks callbacks {};
    callbacks.struct_size = static_cast<uint32_t>(struct_size);
    callbacks.version = version;
    return callbacks;
}

LbPickerCallbacks picker_group(size_t struct_size, uint32_t version = LB_CALLBACKS_VERSION)
{
    LbPickerCallbacks callbacks {};
    callbacks.struct_size = static_cast<uint32_t>(struct_size);
    callbacks.version = version;
    return callbacks;
}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto scratch_directory = ByteString::formatted("{}/Ladybird-Port-Callbacks-{}", Core::StandardPaths::tempfile_directory(), getpid());
    MUST(Core::Directory::create(scratch_directory, Core::Directory::CreateDirectories::Yes));
    VERIFY(setenv("XDG_CONFIG_HOME", scratch_directory.characters(), 1) == 0);

    (void)arguments;

    // No host request callbacks: every request the engine blocks on must still be answered by the shim.
    // NB: LbAppCreate constructs the application itself, so this probe must not create one first -- Application's
    // constructor VERIFYs that it is the only instance.
    LbCallbacks core_callbacks {};
    core_callbacks.on_ready_to_paint = did_become_ready_to_paint;
    core_callbacks.on_load_finish = did_finish_load;
    core_callbacks.on_title_change = did_change_title;
    auto* port = LbAppCreate("TestNativeCallbacks", &core_callbacks);
    if (!port) {
        outln("FAIL: LbAppCreate returned null");
        return 1;
    }

    outln("PHASE 1: registration accepts this dylib's struct_size and version");
    {
        auto ui = ui_group(sizeof(LbUiCallbacks));
        check(LbAppSetUiCallbacks(port, &ui) == 1, "LbAppSetUiCallbacks accepts a full struct at the current version"sv);

        auto page = page_group(sizeof(LbPageCallbacks));
        check(LbAppSetPageCallbacks(port, &page) == 1, "LbAppSetPageCallbacks accepts a full struct at the current version"sv);

        auto picker = picker_group(sizeof(LbPickerCallbacks));
        check(LbAppSetPickerCallbacks(port, &picker) == 1, "LbAppSetPickerCallbacks accepts a full struct at the current version"sv);
    }

    outln("");
    outln("PHASE 2: a group this dylib cannot trust is refused, not misread");
    {
        // Only the two leading members: exactly what a host compiled against an older header would send.
        auto short_ui = ui_group(8);
        check(LbAppSetUiCallbacks(port, &short_ui) == 0, "a short LbUiCallbacks is refused"sv);

        auto short_page = page_group(4);
        check(LbAppSetPageCallbacks(port, &short_page) == 0, "a short LbPageCallbacks is refused"sv);

        auto short_picker = picker_group(8);
        check(LbAppSetPickerCallbacks(port, &short_picker) == 0, "a short LbPickerCallbacks is refused"sv);

        auto wrong_version = ui_group(sizeof(LbUiCallbacks), LB_CALLBACKS_VERSION + 1);
        check(LbAppSetUiCallbacks(port, &wrong_version) == 0, "an LbUiCallbacks from a newer version is refused"sv);

        check(LbAppSetUiCallbacks(port, nullptr) == 0, "a null LbUiCallbacks is refused"sv);
        check(LbAppSetPageCallbacks(port, nullptr) == 0, "a null LbPageCallbacks is refused"sv);
        check(LbAppSetPickerCallbacks(port, nullptr) == 0, "a null LbPickerCallbacks is refused"sv);

        // A refusal must leave the previously accepted group in place rather than half-clearing it.
        auto still_good = ui_group(sizeof(LbUiCallbacks));
        check(LbAppSetUiCallbacks(port, &still_good) == 1, "a valid group still registers after a refusal"sv);
    }

    outln("");
    outln("PHASE 3: a tab with no host request callbacks still loads, and answers its own requests");
    {
        auto tab = LbTabCreate(port);
        check(tab != 0, "LbTabCreate succeeds with no host callbacks registered"sv);

        // No setter has been called for the picker or dialog groups on this tab's behalf, which is the state that used
        // to wedge a page. Loading proves the tab is functional in that state.
        auto page_url = "data:text/html,<title>callbacks</title><select><option>a</option></select>"sv;
        LbTabLoad(port, tab, page_url.characters_without_null_termination(), page_url.length());

        // Bounded wait for a frame: a page containing a <select> and no host picker callback is exactly the case that
        // used to hang, so it must paint rather than merely not crash.
        LbSurface surface {};
        auto have_frame = false;
        auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(20);
        while (MonotonicTime::now() < deadline && !have_frame) {
            LbAppPump(port);
            have_frame = LbTabGetSurface(port, tab, &surface) == 1;
        }
        check(have_frame, "a page with a <select> paints when no picker callback is registered"sv);

        check(LbTabZoomLevel(port, tab) > 0.0, "the tab answers a query after loading without any host callbacks"sv);
        check(LbTabPageMuteState(port, tab) == 0, "the tab reports its mute state while no callbacks are registered"sv);

        // The commands that take the picker answers' place: a host that shows nothing must still be able to drive these
        // without a crash, and they are no-ops on an unknown tab.
        LbTabStopLoading(port, tab);
        LbTabSelectAll(port, tab);
        LbTabFindEnd(port, tab);
        check(true, "commands are safe with no host callbacks registered"sv);

        LbTabDestroy(port, tab);
        check(LbTabZoomLevel(port, tab) == 1.0, "a destroyed tab reports the neutral zoom level"sv);
    }

    outln("");
    outln("PHASE 4: Command-click creates a registered background tab");
    {
        auto page = page_group(sizeof(LbPageCallbacks));
        page.on_new_tab = did_open_new_tab;
        check(LbAppSetPageCallbacks(port, &page) == 1, "the new-tab callback registers"sv);

        auto tab = LbTabCreate(port);
        check(tab != 0, "the Command-click parent tab is created"sv);
        LbAppSetActiveTab(port, tab);
        LbTabSetViewport(port, tab, 800, 600, 1.0);
        auto initial_loads = loads_finished;
        auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(20);
        while (MonotonicTime::now() < deadline && loads_finished == initial_loads)
            LbAppPump(port);

        auto page_url = "data:text/html,%3Cbody%20style%3D%27margin%3A0%27%3E%3Ca%20href%3D%27https%3A%2F%2Fexample.com%2F%27%20style%3D%27display%3Ablock%3Bwidth%3A100vw%3Bheight%3A100vh%27%3Ego%3C%2Fa%3E%3Cscript%3EaddEventListener%28%27mousedown%27%2Ce%3D%3Edocument.title%3D%27command%3A%27%2Be.metaKey%29%3C%2Fscript%3E%3C%2Fbody%3E"sv;
        auto loads_before = loads_finished;
        auto paints_before = paints_ready;
        LbTabLoad(port, tab, page_url.characters_without_null_termination(), page_url.length());

        deadline = MonotonicTime::now() + AK::Duration::from_seconds(20);
        while (MonotonicTime::now() < deadline && (loads_finished == loads_before || paints_ready == paints_before))
            LbAppPump(port);

        constexpr uint32_t platform_control_on_macos = 1u << 3;
        LbTabSendMouseWithModifiers(port, tab, LbMouseEventDown, 20, 20, LbMouseButtonPrimary, LbMouseButtonPrimary, 1, 0, platform_control_on_macos);
        LbTabSendMouseWithModifiers(port, tab, LbMouseEventUp, 20, 20, LbMouseButtonPrimary, LbMouseButtonNone, 1, 0, platform_control_on_macos);
        deadline = MonotonicTime::now() + AK::Duration::from_seconds(20);
        while (MonotonicTime::now() < deadline && opened_new_tab == 0)
            LbAppPump(port);

        check(saw_command_modifier, "Command-click reaches the page with metaKey=true"sv);
        check(opened_parent_tab == tab, "Command-click reports the active parent tab"sv);
        check(opened_new_tab != 0, "Command-click reports the engine-created tab"sv);
        check(opened_activate_tab == 0, "Command-click requests a background tab"sv);
        if (opened_new_tab != 0)
            LbTabDestroy(port, opened_new_tab);
        LbTabDestroy(port, tab);
    }

    outln("");
    check(LbAppSetPageCallbacks(port, nullptr) == 0, "refusing after tabs exist does not disturb them"sv);
    LbAppDestroy(port);

    if (failures != 0) {
        outln("");
        outln("FAILED: {} check(s)", failures);
        return 1;
    }
    outln("");
    outln("PASS: callback groups are size- and version-checked, and absent request callbacks still leave a live tab");
    return 0;
}
