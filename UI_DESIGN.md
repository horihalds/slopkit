# UI & Design Rules

These rules apply to all user interface work in this project. Read this file before adding or changing any UI code.

## 1. Stack

- The UI is built with Dear ImGui using the GLFW + OpenGL backends (`imgui_impl_glfw`, `imgui_impl_opengl3`). Do not introduce other UI frameworks or backends.
- Target platform is Linux with **Wayland** as the primary display server (see section 7).
- UI code follows the project's C++ standard (C++23); see `AGENTS.md`.

## 2. Visual Direction

- Modern and clean: generous padding and spacing, subtle rounding, minimal borders, a restrained color palette with a single accent color, and consistent alignment.
- Avoid stock ImGui styling. Configure `ImGuiStyle` centrally (rounding, padding, spacing, colors) rather than tweaking it per widget.
- Prefer whitespace and hierarchy (size, weight, muted color) over lines and boxes to separate content.
- Keep motion subtle. Short hover/active transitions are fine; avoid anything flashy.

## 3. Theming

- Dark theme is the default. A light theme must also be available and switchable at runtime.
- Define themes as data: a struct of named semantic colors (`background`, `surface`, `text`, `text_muted`, `accent`, `border`, and so on). Apply them through a single `apply_theme()` function.
- Never hard-code colors in widget code. Always pull from the active theme.
- When adding a new color role, add it to both the dark and the light theme.
- Both themes must keep readable contrast for text and for interactive states (hover, active, disabled).

## 4. Custom Components

- Build reusable widgets instead of repeating raw ImGui calls. Put them in a dedicated module (e.g. `ui/components/`): buttons, toggles, cards, panels, tooltips, section headers, input fields, and similar.
- Components take their styling from the active theme, expose a small and consistent API, and keep no hidden global state.
- Use `ImGui::PushID` / `PopID` or explicit IDs to avoid ID collisions.
- Prefer composing existing components over adding one-off widgets. If similar ImGui code appears twice, extract it into a component.
- Draw custom visuals with `ImDrawList`, and handle hover, active, focused, and disabled states.
- Component sizes and spacing derive from the style/scale (section 6), never from literal pixel values.

## 5. Fonts

- Use **Noto Sans** as the UI font. Embed it in the binary rather than loading it from disk at runtime, so the app has no external font file dependency.
- Embedding happens at build time through a CMake script that converts the `.ttf` into a generated C++ header. Do not commit generated headers or paste font byte arrays into source files.
- Keep the source font under `assets/fonts/` (e.g. `NotoSans-Regular.ttf`) and track it in version control together with its license (OFL).
- The CMake script (e.g. `cmake/EmbedFont.cmake`) should:
  - Read the file with `file(READ ... HEX)` and convert the hex string into a comma-separated `0x..` byte list.
  - Write the result into the build directory (`${CMAKE_BINARY_DIR}/generated/`) as a header containing a `constexpr unsigned char[]` and a size constant.
  - Re-run automatically when the font file changes (e.g. `set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS <font>)`, or `add_custom_command` with `DEPENDS`).
  - Be exposed as a function such as `embed_font(<target> <font_path> <symbol_name>)` that also adds the generated include directory to the target.
- Load the embedded font with `ImFontAtlas::AddFontFromMemoryTTF` and set `FontDataOwnedByAtlas = false`, since the data is static and must not be freed by ImGui.
- Additional weights (Bold, Medium) or icon glyphs go through the same `embed_font` function; do not add a separate embedding mechanism.
- Noto Sans covers Latin, Cyrillic, and Greek, but ImGui loads only a default glyph range. If other scripts are needed, request the glyph ranges explicitly.

## 6. DPI and Scaling

The UI must look correct on standard, HiDPI, and fractionally scaled displays, and must update when the scale changes (for example when the window moves to another monitor).

- Use the window content scale (`glfwGetWindowContentScale`) as the source of truth. Listen to `glfwSetWindowContentScaleCallback` and re-apply scaling when it changes.
- Keep the framebuffer size and the window size separate. Use `glfwGetFramebufferSize` for `glViewport`, and let the ImGui GLFW backend handle `DisplayFramebufferScale`. Never assume window size equals framebuffer size.
- Keep an unscaled base `ImGuiStyle` and derive the active style from it with `ScaleAllSizes(scale)` each time the scale changes. Never scale an already scaled style, because that compounds.
- Scale fonts with the same factor. Depending on the ImGui version, either rebuild the atlas at `base_size * scale`, or use the dynamic font sizing (`style.FontScaleDpi`) if available. Do this between frames, never mid-frame.
- Specify all sizes, paddings, and spacings in logical units (the base design at scale 1.0) and multiply by the current scale. No raw pixel literals in UI code.
- Support fractional scales (1.25, 1.5, 1.75, and so on) without blurry text or misaligned layout. Do not round the scale to an integer.
- Do not hard-code assumptions about a single monitor's DPI.

## 7. Wayland

Wayland is the primary target. The app must run natively on Wayland, with X11/XWayland as a fallback only.

- Build GLFW 3.4 or newer with Wayland support (`GLFW_BUILD_WAYLAND=ON`). Prefer the Wayland platform at runtime (`glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND)`), falling back to X11 if Wayland is unavailable. Do not force X11.
- Set the application ID with `glfwWindowHintString(GLFW_WAYLAND_APP_ID, "<app-id>")` so compositors, taskbars, and desktop entries match the window correctly. The ID must match the `.desktop` file name.
- Respect Wayland limitations; do not write code that depends on them:
  - No programmatic window positioning (`glfwSetWindowPos` is a no-op). Never rely on or persist window position.
  - No global cursor positioning or warping outside the window. Use `GLFW_CURSOR_DISABLED` for relative-motion needs.
  - Window focus requests and raising may be ignored by the compositor.
  - Clipboard and drag-and-drop go through GLFW and ImGui's standard paths. Do not talk to the display server directly.
- Window decorations may be client-side (libdecor) or server-side depending on the compositor. Do not draw custom title bars unless explicitly requested, and do not assume a fixed decoration size.
- Scaling on Wayland is per-output and can change at runtime. Follow section 6 and test with fractional scaling enabled.
- Do not use X11-specific APIs (`glfwGetX11Window`, Xlib, `GLFW_EXPOSE_NATIVE_X11`) in UI code. Keep any platform-specific code isolated behind a small interface.

## 8. Window and Layout

- The window must be resizable. Handle framebuffer resize events and keep the viewport correct.
- Layouts must adapt to window size: use relative sizing, `ImGui::GetContentRegionAvail()`, and tables or columns rather than fixed pixel sizes.
- Set a sensible minimum window size with `glfwSetWindowSizeLimits`, expressed in logical units and scaled by the current content scale.
- Keep rendering correct and responsive during interactive resize (redraw from the resize callback if needed, and avoid layout jumps).
- Restore window size (not position) between sessions if persistence is implemented.

## 9. Working Rules for Agents

- Reuse existing components and theme colors before writing new UI code. Add to the component library rather than inlining.
- Do not restyle or refactor existing UI that is unrelated to the task; apply these rules to new and modified code.
- When a change affects appearance, verify it in both dark and light themes, at more than one window size, and at more than one scale factor.
- If a rule here conflicts with what a task requires, follow the task and mention the conflict instead of silently deviating.
