# UI & Design Rules

These rules apply to all user interface work in this project. Read this file before adding or changing any UI code.

## 1. Stack

- The UI is built with **Qt 6 Widgets** (`Qt6::Core`, `Qt6::Gui`, `Qt6::Widgets`) in C++23. No QML, no Qt stylesheets (QSS) and no other UI toolkit.
- The application runs on the Fusion style (`QStyleFactory::create("Fusion")`) so both themes render identically regardless of the desktop's platform theme.
- Target platform is Linux with **Wayland** as the primary display server (see section 7).
- UI code follows the project's C++ standard (C++23); see `AGENTS.md`.
- The project ships two UI binaries: the main `slopkit` application and the `slopkit-sandbox` practice target launched from the Help menu. Both link the shared `slopkit_ui_common` static library (`ui/theme`, `ui/fonts`, `ui/settings` and `ui/components/`), so the sandbox renders with the same palette, fonts and components instead of duplicating them.

## 2. Visual Direction

- Modern and clean: generous padding and spacing, minimal borders, a restrained color palette with a single accent color, and consistent alignment.
- Do not hand-style widgets. Colors and surfaces come from the application palette; geometry comes from layouts and size hints. Do not hard-code pixel paddings or sizes.
- Prefer whitespace and hierarchy (size, weight, muted color) over lines and boxes to separate content.
- Keep motion subtle. Qt's standard control animations are fine; avoid anything flashy.

## 3. Theming

- Dark theme is the default. A light theme must also be available and switchable at runtime.
- Define themes as data: `ui::Theme` is a struct of named semantic colors (`background`, `surface`, `surface_hover`, `text`, `text_muted`, `accent`, `accent_hover`, `accent_active`, `on_accent`, `border`, `success`, `warning`, `error`). Both `dark_theme()` and `light_theme()` must define every role.
- `make_palette(const Theme&)` maps the roles onto the palette roles Qt paints from (`Window`, `Base`, `Button`, `Text`, `Mid`, `Highlight`, `HighlightedText`, …) for all three color groups.
- `apply_theme(const Theme&)` installs that palette application-wide, updates `active_theme()` and keeps `QStyleHints::setColorScheme` in step so native parts (file dialogs, decorations) follow.
- Never hard-code colors in widget code. Always pull from the active theme, either through the palette or through `widgets::status_color()`.
- When adding a new color role, add it to both the dark and the light theme.
- Both themes must keep readable contrast for text and for interactive states (hover, active, disabled).
- Widgets that paint themselves with an accent or status color (the primary button, the status label) must re-apply their palette when the application palette changes, so a theme switch updates them live.

## 4. Custom Components

- Build reusable widgets instead of repeating raw Qt calls. Put them in a dedicated module (`ui/components/`): buttons, panels, section headers, status labels and icon loading.
- Icons are rasterised at configure time from `assets/icons/*.svg` into multi-size PNGs and embedded as Qt resources; load them through `widgets::application_icon()` / `widgets::action_icon()`, never from disk or a theme at runtime.
- Components take their styling from the active theme, expose a small and consistent API, and keep no hidden global state.
- Prefer composing existing components over adding one-off widgets. If similar Qt code appears twice, extract it into a component.
- For a single-line prompt, reuse `ui::widgets::InputBox` instead of `QInputDialog`: it is themed and parent-owned, runs an optional validator on every edit and again on accept (disabling OK and showing the validator's message while the text is invalid), and can render its field in the monospace font for addresses and expressions.
- Let Qt do the work: use `QSplitter` for dividers, `QGroupBox`/`widgets::Panel` for groups, `QProgressBar` for progress and `QTableView` with a `QAbstractTableModel` for tabular data rather than custom-painted equivalents.
- Model/view code keeps its state in the model; the view is only configured once at construction.
- The first sanctioned exception to the table rule above is the Memory Viewer's hex view: `ui::components::MemoryView` is a custom-painted `QAbstractScrollArea` because its per-byte change colours, its per-column hex offset header, its column width and value groupings that change with the chosen value format and text encoding, its unbounded scrolling, its inline per-cell editor and its right-click menu (value format, text encoding, text-column visibility, Go To and Back) cannot be expressed by a `QTableView` plus a column/delegate model. It sits in the Memory Viewer's lower pane (there is no loading or status row below it) and it still keeps the model/view spirit: the cache, the format/encoding, the three-block live window and the write submission live in `ui::components::MemoryViewDocument`, and the widget only lays out, paints and handles input.
- The second sanctioned exception is the Memory Viewer's instruction listing: `ui::components::DisassemblyView` is likewise custom-painted because each row's byte text has a different width and wraps onto as many lines as the bytes column is wide (the column gives up space first, down to a single byte per line, while the instruction text keeps what is left and clips), so rows have variable heights, the address column is module-relative, `??`/`.byte` rows paint muted and the instruction cursor is unbounded with its own key and wheel handling (including stepping to the next page-aligned window at the decoded bounds), none of which a `QTableView` plus a column/delegate model covers cleanly. Its live window, decode cache, wrapped byte lines and rendered row text live in `ui::components::DisassemblyDocument`, and the widget only lays out, paints and handles input.

- The disassembly listing renders every address an instruction prints — a relative branch/call target or a computed memory operand — the way it renders the row's own address: `<module>+<RVA>` when the target lands in a file-backed image, and the decoded absolute text otherwise. The form follows Settings > Addresses, so Absolute address mode keeps the Zydis text, and the listing, the painted text and the Copy forms all agree.
- Each Memory Viewer pane keeps its own navigation history. The instruction listing's right-click menu carries `Follow` and `Follow in Memory View` for a decoded row whose instruction references an address (the first reference is used; a `.byte`, `??` or unreadable row has neither entry); both request a fresh live page, the first moves the listing's own cursor there and the second moves the byte view's while the listing stays put. Each pane's menu also carries a `Back`, always present and disabled while that pane's history is empty, which returns it to the location it jumped from. Only an explicit jump (`Follow`, `Follow in Memory View`, `Go To...` and the pointer-chain evaluation) records; wheel/keyboard scrolling, re-layout and window-open seeding do not, and opening the window at an address clears both histories.

## 5. Fonts

- Use **Noto Sans** as the UI font. Embed it in the binary rather than loading it from disk at runtime, so the app has no external font file dependency.
- Use **Noto Sans Mono** as the companion monospace font for text that must line up (addresses, hex dumps, paths, sized fields) and apply it only to those widgets.
- Embedding happens at build time through a CMake script that converts the `.ttf` into a generated C++ header. Do not commit generated headers or paste font byte arrays into source files.
- Keep the source font under `assets/fonts/` (e.g. `NotoSans-Regular.ttf`) and track it in version control together with its license (OFL).
- The CMake script (e.g. `cmake/EmbedFont.cmake`) should:
  - Read the file with `file(READ ... HEX)` and convert the hex string into a comma-separated `0x..` byte list.
  - Write the result into the build directory (`${CMAKE_BINARY_DIR}/generated/`) as a header containing a `constexpr unsigned char[]` and a size constant.
  - Re-run automatically when the font file changes (e.g. `set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS <font>)`, or `add_custom_command` with `DEPENDS`).
  - Be exposed as a function such as `embed_font(<target> <font_path> <symbol_name>)` that also adds the generated include directory to the target.
- Register the embedded faces with `QFontDatabase::addApplicationFontFromData` and expose them as `ui::ui_font()` / `ui::mono_font()`; the application font is set once, at start-up.
- Additional weights (Bold, Medium), a monospace companion or icon glyphs go through the same `embed_font` function; do not add a separate embedding mechanism.
- Qt selects glyph coverage from the font itself, so no glyph ranges need to be requested explicitly.

## 6. DPI and Scaling

The UI must look correct on standard, HiDPI, and fractionally scaled displays, and must update when the scale changes (for example when the window moves to another monitor).

- Qt owns device-pixel-ratio and fractional scaling. Do not read a scale factor, do not multiply sizes by one, and do not re-create widgets when the scale changes.
- Specify all sizes, paddings, spacing and fonts in logical units (the base design at scale 1.0). No raw pixel literals that encode a scale.
- Never assume widget size equals framebuffer size, and never compute it; layouts and the platform handle the conversion.
- Support fractional scales (1.25, 1.5, 1.75, and so on) without blurry text or misaligned layout. Do not round the scale to an integer.
- Do not hard-code assumptions about a single monitor's DPI. Use layouts that adapt and a sensible minimum size instead.

## 7. Wayland

Wayland is the primary target. The app must run natively on Wayland, with X11/XWayland as a fallback only.

- Set `QGuiApplication::setDesktopFileName("slopkit")` so compositors, taskbars and desktop entries match the window correctly. The ID must match the `.desktop` file name and its `StartupWMClass`.
- Load the window icon from the generated `:/icons/<N>x<N>/apps/slopkit.png` resource so every size is available to the compositor.
- Menu action glyphs are embedded the same way under `:/icons/actions/<N>x<N>/<name>.png` and reached through `widgets::action_icon()`.
- Respect Wayland limitations; do not write code that depends on them:
  - No programmatic window positioning. Never rely on or persist window position, with the single documented exception of the Memory Viewer below.
  - A dialog in its own OS window is positioned by the compositor at first show; do not compute, save or restore its geometry. The Memory Viewer is the one deliberate exception: it is a detached, non-transient top-level window whose size is remembered across sessions (and whose position is restored only where the platform honours it - on Wayland the compositor may still place it itself), so the investigation layout survives a restart. Every other dialog stays compositor-owned and unpersisted.
  - No global cursor positioning or warping outside the window.
  - Window focus requests and raising may be ignored by the compositor.
  - Clipboard and drag-and-drop go through Qt's standard paths. Do not talk to the display server directly.
- Window decorations may be client-side or server-side depending on the compositor. Do not draw custom title bars unless explicitly requested, and do not assume a fixed decoration size.
- Scaling on Wayland is per-output and can change at runtime. Follow section 6 and test with fractional scaling enabled.
- Do not use X11-specific APIs in UI code. Keep any platform-specific code isolated behind a small interface.

## 8. Window and Layout

- The main window is a `QMainWindow` with a menu bar, a single full-width scan-progress bar above the central split zones, and a status bar; the window must be resizable and carry a sensible minimum size.
- The menu bar holds exactly `File`, `View` and `Help`. `File` holds the process/table commands, `View` holds `Log` then `Settings`, and `Help` holds `Launch Practice Target` then `About slopkit`; there is no `Edit` menu, and `Undo Scan` / `Add Address Manually...` are scanner-panel buttons only.
- The file commands are reachable from the menus and carry window-scoped shortcuts: `Ctrl+T` picks the process, `Ctrl+O` opens an address table and `Ctrl+S` saves it; they fire only while the main window is focused.
- The Memory Viewer binds `Ctrl+G` (window-scoped) to its `Go To...` command, so it opens the same prompt from anywhere in the dialog while it is focused; the prompt targets the pane the keyboard focus is in - the disassembly listing when the focus is inside it, otherwise the byte view - so one command serves both panes. It adds no shortcut to and shadows none of the main window's. Its panes also add `Follow`, `Follow in Memory View` and a per-pane `Back` to their context menus (section 4).
- Layouts must adapt to window size: use layouts and `QSplitter` stretch factors rather than fixed sizes. The scan zone is pinned to the scanner panel's content height (the vertical splitter gives it no stretch) and the address list absorbs every remaining pixel, which keeps the hits table's bottom edge level with the `Memory Scan Options` panel; the scan zone splits 50/50 between the found list and the scanner. The Memory Viewer is itself a three-pane split: a vertical `QSplitter` keeps the byte view in the lower pane (30 of the 100 units) below a 70-unit upper zone that a horizontal `QSplitter` divides 70/30 between the disassembly listing and the debugger stats pane; the ratio is seeded once on the first show and held by the stretch factors on every resize, and every handle is non-collapsible so no pane can vanish.
- The status bar carries the attached-process label in its left slot and the address list's status line in its permanent right slot; the scan progress is a single `QProgressBar` spanning the top of the window above the split zones (a deliberate change from the old status-bar progress bar). This deliberately replaces the earlier rule that the status bar showed only the attached-process label; the process list and other dialogs never duplicate either.
- The dialogs (`Process List`, `Add Address`, `Memory Viewer`, `Log`, `Settings`) are `QDialog` top-level windows with their own decorations and taskbar/Alt-Tab entry, can move to another monitor, and are owned by the main window. The Memory Viewer alone is built with no parent widget, so no window-manager transient hint keeps it stacked above the main window; every other dialog stays a transient child.
- The `Process List` picker is **application-modal**: while it is open the main window accepts no keyboard or mouse input, so a half-chosen target cannot be interacted with behind it. This deliberately overrides the non-modal rule below for this dialog only.
- The `Process List` picker is also a **fixed-size** chooser: it locks its 600x440 default size and offers no minimize or maximize affordance, so it reads as a small picker rather than a resizable window. This deliberately overrides the compositor-owned dialog size rule below. A Wayland compositor may still draw a minimize affordance for a toplevel; the locked size makes maximize a no-op there regardless.
- In the `Process List` picker the filter box owns the keyboard: `Up`/`Down` step the highlighted target through the current filtered/sorted view, cycling at both ends, and `Enter` attaches the highlighted row — so a target can be picked without leaving the filter box. Only unmodified `Up`/`Down` are consumed; every other key and modifier combination stays with the line edit.
- The other dialogs (`Add Address`, `Memory Viewer`, `Log`, `Settings`) stay **non-modal**: the main window keeps taking input while they are open.
- Treat a dialog's position and size as compositor-owned: never save, restore or compute them; nothing is persisted between runs. The Memory Viewer is the single deliberate exception: its size (and its position where the platform honours it) is saved in the settings store whenever it is hidden and restored when it is created again, so the investigation layout survives a restart.
- Do not draw custom title bars for dialogs (see section 7). The window-manager close button has the same effect as an in-dialog close button, and the dialog can be re-opened at any time.
- Keep the app idle-quiet: updates are event-driven and the only periodic timer is the low-frequency scan-progress/freeze tick, which also drives the live value pass (`ui::LiveValues::poll()`). The pass submits nothing while live update is off, while no target is attached, while there is nothing displayed or while a pass is still in flight, so an idle window stays silent. The same tick re-resolves the address list's stored expressions on a slower internal cadence (about once a second, and immediately when an expression or the module map changes), still as one batched job and still nothing while no target is attached or a resolve is in flight, so no second timer is ever created.
- The main window's Tab order is fixed at construction and walks the scan flow: First Scan → Next Scan → Undo Scan → Cancel → Hex → Value → Upper value → scan type → value type → memory region → Start address → Stop address → Writable → Executable → CopyOnWrite → Fast Scan → alignment → Pause the game while scanning → the hits table → Memory View → Add Address Manually → Table Settings → the address list. Each panel owns the run it builds (through `ui::widgets::chain_tab_order()`), while `MainWindow::apply_tab_order()` links only the hops between panels; a hidden control keeps its slot (Qt skips it while tabbing), and the UI test asserts the whole order so a layout edit cannot silently move a control.
- Re-activating the main window (alt-tabbing back to it) moves the keyboard focus into the scanner `Value` field and selects its whole content, so a new value can be typed immediately. `Enter` in that field runs the scan — `First Scan` while no scan has produced a result set yet, `Next Scan` once one exists — through the same guards and log records as the buttons; it never performs the `New Scan` reset. The focus is a no-op when the field is disabled (scan types that need no value).

## 9. Threading and Target Access

- UI code must never touch a `process::Session` and must never call a plugin or perform a syscall. Every target access — process listing, attach, module/thread probing, the desktop-entry index, memory reads, memory writes and the freeze pass — is submitted to the background `process::AccessWorker`.
- Submit a job and stay responsive: the last known data stays visible while a job is in flight, and the affected control is disabled and relabelled or a small busy label reports the work (the Process List's `Attaching...` button). The Process List picker is the deliberate exception: it has no status line any more and reports a still-current failure on its Details message line instead. The scanner panel and the Memory Viewer are the other exceptions: neither carries a status line - the Memory Viewer hosts three panes and none of them adds one - and they report through the log window (`View > Log`). The completion is applied by `AccessWorker::drain()` on the UI thread.
- The worker reports a queued completion through its completion hook, which only calls `ui::CompletionNotifier::post()`; the notifier coalesces the wake-up and the queued Qt event drains the completions. The hook runs on the worker thread and must never touch a widget.
- Match every completion against the pending job id and ignore stale results (a changed selection, target, page or removed entry), so an out-of-order result never corrupts the view.
- The live value pass batches the addresses the visible surfaces show into one `submit_read_many` job, so one interval costs one job and one session lock; a per-address failure never aborts the rest of the batch. Stored address expressions are re-resolved the same way: one batched `submit_resolve_expressions` job per interval, keyed by entry id, where each failure keeps the entry's last good address and is reported once per state change.
- Only the UI thread touches widgets; the worker thread touches neither.

## 10. Working Rules for Agents

- Reuse existing components and theme colors before writing new UI code. Add to the component library rather than inlining.
- Do not restyle or refactor existing UI that is unrelated to the task; apply these rules to new and modified code.
- When a change affects appearance, verify it in both dark and light themes, at more than one window size, and at more than one scale factor.
- If a rule here conflicts with what a task requires, follow the task and mention the conflict instead of silently deviating.
