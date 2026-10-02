# UI & Design Rules

These rules apply to all user interface work in this project. Read this file before adding or changing any UI code.

## 1. Stack

- The UI is built with **Qt 6 Widgets** (`Qt6::Core`, `Qt6::Gui`, `Qt6::Widgets`) in C++23. No QML, no Qt stylesheets (QSS) and no other UI toolkit.
- The application runs on the Fusion style (`QStyleFactory::create("Fusion")`) so both themes render identically regardless of the desktop's platform theme.
- Target platform is Linux with **Wayland** as the primary display server (see section 7).
- UI code follows the project's C++ standard (C++23); see `AGENTS.md`.

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

- Build reusable widgets instead of repeating raw Qt calls. Put them in a dedicated module (`ui/components/`): buttons, panels, collapsible sections, section headers, status labels and icon loading.
- Components take their styling from the active theme, expose a small and consistent API, and keep no hidden global state.
- Prefer composing existing components over adding one-off widgets. If similar Qt code appears twice, extract it into a component.
- Let Qt do the work: use `QSplitter` for dividers, `QGroupBox`/`CollapsibleSection` for groups, `QProgressBar` for progress and `QTableView` with a `QAbstractTableModel` for tabular data rather than custom-painted equivalents.
- Model/view code keeps its state in the model; the view is only configured once at construction.

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
- Respect Wayland limitations; do not write code that depends on them:
  - No programmatic window positioning. Never rely on or persist window position.
  - A dialog in its own OS window is positioned by the compositor at first show; do not compute, save or restore its geometry.
  - No global cursor positioning or warping outside the window.
  - Window focus requests and raising may be ignored by the compositor.
  - Clipboard and drag-and-drop go through Qt's standard paths. Do not talk to the display server directly.
- Window decorations may be client-side or server-side depending on the compositor. Do not draw custom title bars unless explicitly requested, and do not assume a fixed decoration size.
- Scaling on Wayland is per-output and can change at runtime. Follow section 6 and test with fractional scaling enabled.
- Do not use X11-specific APIs in UI code. Keep any platform-specific code isolated behind a small interface.

## 8. Window and Layout

- The main window is a `QMainWindow` with a menu bar, a status bar and the splitter-based central widget; the window must be resizable and carry a sensible minimum size.
- The file commands are reachable from the menus and carry window-scoped shortcuts: `Ctrl+T` picks the process, `Ctrl+O` opens an address table and `Ctrl+S` saves it; they fire only while the main window is focused.
- Layouts must adapt to window size: use layouts and `QSplitter` stretch factors rather than fixed sizes, and keep the previous zone proportions (62 % scan zone / 38 % address list; the scan zone splits 50/50 between the found list and the scanner).
- The status bar shows the attached-process label and the scan progress; the process list and other dialogs never duplicate them.
- The dialogs (`Process List`, `Add Address`, `Memory Viewer`, `Settings`) are non-modal `QDialog` top-level windows with their own decorations and taskbar/Alt-Tab entry, can move to another monitor, and are owned by the main window.
- Treat a dialog's position and size as compositor-owned: never save, restore or compute them; nothing is persisted between runs.
- Do not draw custom title bars for dialogs (see section 7). The window-manager close button has the same effect as an in-dialog close button, and the dialog can be re-opened at any time.
- Keep the app idle-quiet: updates are event-driven and the only periodic timer is the low-frequency scan-progress/freeze tick.

## 9. Threading and Target Access

- UI code must never touch a `process::Session` and must never call a plugin or perform a syscall. Every target access — process listing, attach, module/thread probing, the desktop-entry index, memory reads, memory writes and the freeze pass — is submitted to the background `process::AccessWorker`.
- Submit a job and stay responsive: the last known data stays visible while a job is in flight, the affected control is disabled and relabelled (for example `Refresh` shows `Refreshing...`), and the completion is applied by `AccessWorker::drain()` on the UI thread.
- The worker reports a queued completion through its completion hook, which only calls `ui::CompletionNotifier::post()`; the notifier coalesces the wake-up and the queued Qt event drains the completions. The hook runs on the worker thread and must never touch a widget.
- Match every completion against the pending job id and ignore stale results (a changed selection, target, page or removed entry), so an out-of-order result never corrupts the view.
- Only the UI thread touches widgets; the worker thread touches neither.

## 10. Working Rules for Agents

- Reuse existing components and theme colors before writing new UI code. Add to the component library rather than inlining.
- Do not restyle or refactor existing UI that is unrelated to the task; apply these rules to new and modified code.
- When a change affects appearance, verify it in both dark and light themes, at more than one window size, and at more than one scale factor.
- If a rule here conflicts with what a task requires, follow the task and mention the conflict instead of silently deviating.
