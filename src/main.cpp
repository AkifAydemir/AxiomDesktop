#include "axiom.hpp"
#include "win32_handle.hpp"
#include <windows.h>
#include <chrono>
#include <exception>
#include <string>
#include <thread>
namespace {
[[nodiscard]] bool activate_existing_instance() {
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (HWND window = FindWindowW(axiom::axiom_window_class_name, nullptr); window != nullptr) {
            PostMessageW(window, axiom::axiom_activate_message, 0, 0);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return false;
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    axiom::UniqueWin32Handle instance_mutex{
        CreateMutexW(nullptr, FALSE, L"Local\\AxiomDesktop.SingleInstance.v1")};
    if (!instance_mutex) {
        MessageBoxW(nullptr, L"Axiom could not create its single-instance mutex.",
                    L"Axiom startup failure", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        (void)activate_existing_instance();
        return 0;
    }
    try {
        axiom::App app{instance};
        return app.run();
    } catch (const std::exception &exception) {
        const std::string narrow = exception.what();
        const std::wstring message{narrow.begin(), narrow.end()};
        MessageBoxW(nullptr, message.c_str(), L"Axiom startup failure", MB_OK | MB_ICONERROR);
        return 1;
    } catch (...) {
        MessageBoxW(nullptr, L"Axiom failed to start because of an unknown error.",
                    L"Axiom startup failure", MB_OK | MB_ICONERROR);
        return 1;
    }
}
