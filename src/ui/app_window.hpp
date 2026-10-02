#pragma once

#include <expected>
#include <memory>
#include <string>

#include <imgui.h>

struct GLFWwindow;

namespace slopkit::ui
{

    // Owns the GLFW window, the OpenGL context and the ImGui backends. Scaling
    // is driven by the window content scale. Created through create() because
    // the GLFW callbacks hold a pointer to the instance.
    class AppWindow
    {
    public:
        AppWindow() = default;
        ~AppWindow();

        AppWindow(const AppWindow&)            = delete;
        AppWindow& operator=(const AppWindow&) = delete;

        static std::expected<std::unique_ptr<AppWindow>, std::string> create();

        [[nodiscard]] GLFWwindow* handle() const noexcept;
        [[nodiscard]] bool        should_close() const noexcept;

        // Current content scale.
        [[nodiscard]] float content_scale() const noexcept;

        // Returns true once after the content scale changed.
        [[nodiscard]] bool take_scale_change() noexcept;

        void set_clear_color(const ImVec4& color) noexcept;

        void poll_events();
        void begin_frame();
        void end_frame();

    private:
        static void framebuffer_size_callback(GLFWwindow* window, int width, int height);
        static void content_scale_callback(GLFWwindow* window, float x_scale, float y_scale);
        static void error_callback(int code, const char* description);

        void shutdown() noexcept;

        GLFWwindow* window_ {nullptr};
        bool        glfw_initialized_ {false};
        bool        imgui_context_ {false};
        bool        imgui_glfw_ {false};
        bool        imgui_opengl_ {false};
        bool        scale_changed_ {false};
        float       scale_ {1.0f};
        ImVec4      clear_color_ {0.0f, 0.0f, 0.0f, 1.0f};
    };

} // namespace slopkit::ui
