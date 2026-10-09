#include <stdafx.hpp>
#include <app/context.hpp>
#include <core/memory/compatibility.hpp>
#include <app/startup_stage.hpp>
#include <app/workers.hpp>
#include <simulation/seed_diagnostics.hpp>
#include <simulation/shot_trace.hpp>
#include <simulation/trace_session.hpp>
#include <render/overlay/ui.hpp>
#include <scripting/runtime.hpp>

#include <timeapi.h>
#include <string_view>
#include <array>
#include <thread>
#include <format>

#pragma comment(lib, "winmm.lib")

namespace features::misc { int auto_accept_report(const char* path); }

namespace
{
    // RAII guard for high-precision multimedia timer intervals
    struct timer_period_guard
    {
        timer_period_guard() noexcept { ::timeBeginPeriod(1); }
        ~timer_period_guard() noexcept { ::timeEndPeriod(1); }
    };

    // RAII guard to prevent running duplicate instances of the process
    struct single_instance_guard
    {
        single_instance_guard() noexcept
        {
            handle = ::CreateMutexW(nullptr, FALSE, L"Local\\vesta.overlay.single-instance");
            acquired = (handle != nullptr && ::GetLastError() != ERROR_ALREADY_EXISTS);
        }

        ~single_instance_guard() noexcept
        {
            if (handle != nullptr)
            {
                ::CloseHandle(handle);
            }
        }

        HANDLE handle{ nullptr };
        bool acquired{ false };
    };

    // Diagnostics tool for parsing alternative entry CLI commands
    bool handle_diagnostic_commands(int argc, char** argv)
    {
#if defined(VESTA_SHOT_TRACE_ENABLED) && VESTA_SHOT_TRACE_ENABLED
        if (argc == 3)
        {
            const std::string_view command{ argv[1] };
            if (command == "--seed-snapshot-report")
            {
                ::ExitProcess(simulation::seed_diagnostics::report(argv[2]));
            }
            if (command == "--compatibility-report")
            {
                ::ExitProcess(game::compatibility::report(argv[2]));
            }
            if (command == "--auto-accept-report")
            {
                ::ExitProcess(features::misc::auto_accept_report(argv[2]));
            }
        }
#endif
        return false;
    }

    // Safely handles the system shutdown loop when cs2.exe closes
    void handle_game_watchdog(HANDLE game_process)
    {
        if (::WaitForSingleObject(game_process, INFINITE) != WAIT_OBJECT_0)
            return;

        // Reset the mouse clip and release the virtual input gateways cleanly
        ::ClipCursor(nullptr);
        app::context().input.set_key_gate(0, false);
        app::context().input.set_movement_gate({}, false);
        
        // Utilize the modern hardware-level batching framework for immediate key releases
        const std::array teardown_sequence{
            platform::windows::input_gateway::key_transition{ VK_ESCAPE, false },
            platform::windows::input_gateway::key_transition{ VK_CONTROL, false },
            platform::windows::input_gateway::key_transition{ VK_F24, false },
        };
        app::context().input.keys(teardown_sequence);
        
        // Ensure mouse pointers are stripped of active selection locks
        app::context().input.pointer(0, 0, platform::windows::pointer_action::primary_up | platform::windows::pointer_action::secondary_up);

        app::context().overlay.request_shutdown();
    }
}

int main(int argc, char** argv)
{
    // Process diagnostics reports before initializing the core window state
    handle_diagnostic_commands(argc, argv);

    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    constexpr DWORD ui_access_success_val = ERROR_SUCCESS;
    DWORD ui_access_result = ui_access_success_val;

#if !((defined(VESTA_WINDOWED_PROFILE) && VESTA_WINDOWED_PROFILE) || (defined(VESTA_WINDOWED_SESSION) && VESTA_WINDOWED_SESSION))
    #if defined(VESTA_INSTALLED_UIACCESS) && VESTA_INSTALLED_UIACCESS
    if (!ui_access::enabled())
    {
        ::MessageBoxW(nullptr,
                      L"Vesta requires the UIAccess token declared in its manifest. "
                      L"Install a trusted, signed build in Program Files.",
                      L"Vesta installation", MB_OK | MB_ICONERROR);
        return EXIT_FAILURE;
    }
    #else
    if (app::select_startup_stage(ui_access::elevated(), ui_access::enabled()) == app::startup_stage::request_elevation)
    {
        return (ui_access::elevate() == ui_access_success_val) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    ui_access_result = ui_access::prepare();
    #endif
#endif

    const single_instance_guard instance{};
    if (!instance.acquired)
    {
        return EXIT_SUCCESS;
    }

    if (!app::context().diagnostics.open(" :> "))
    {
        return EXIT_FAILURE;
    }

    if (ui_access_result != ui_access_success_val)
    {
        const auto message = std::format(L"UIAccess initialization failed (Win32 {}). Vesta requires UIAccess for presentation.", ui_access_result);
        ::MessageBoxW(nullptr, message.c_str(), L"Vesta startup", MB_OK | MB_ICONERROR);
        return EXIT_FAILURE;
    }

    const timer_period_guard timer_period{};

    // Connect dependencies and attach to target memory space
    if (!app::context().process.attach(L"cs2.exe"))
    {
        app::context().diagnostics.error("Process attachment initialization failure: Could not find or open a handle to cs2.exe.");
        return EXIT_FAILURE;
    }
    
    if (!app::context().input.connect())
    {
        app::context().diagnostics.error("Input pipeline connection failure: Gateway hook failed to map hardware devices.");
        return EXIT_FAILURE;
    }

    // Dynamic pattern scanning configuration steps
    if (!app::context().modules.discover(app::context().process))
    {
        app::context().diagnostics.error("Module discovery failure: Failed to parse running imagery within client.dll / engine2.dll namespaces.");
        return EXIT_FAILURE;
    }
    
    if (!app::context().addresses.initialize())
    {
        app::context().diagnostics.error("Address mapping failure: Static dynamic signatures could not cross-reference memory bounds.");
        return EXIT_FAILURE;
    }
    
    if (!game::fields().initialize())
    {
        app::context().diagnostics.error("Schema layout failure: Game networking fields structure offsets could not be resolved.");
        return EXIT_FAILURE;
    }

    // Apply file and scripting structures
    config::apply_default_config();
    if (!config::storage.read_cache())
    {
        app::context().diagnostics.warning("Cache configuration missing. Generating a clean local fallback state.");
        config::storage.write_cache();
    }

    if (!scripting::runtime().initialize())
    {
        app::context().diagnostics.warning("Lua runtime storage could not be initialized; scripting extensions have been disabled.");
        config::general_settings.lua_enabled = false;
    }

    config::publish_runtime_snapshot();
    simulation::trace_session::initialize();

    // Spawn native process attachment watchdog thread
    const auto game_process_handle = static_cast<HANDLE>(app::context().process.native_handle());
    std::thread(handle_game_watchdog, game_process_handle).detach();

    // Async thread allocation matrix for feature workers
    std::thread(app::workers::game).detach();
    std::thread(app::workers::movement).detach();
    std::thread(app::workers::combat).detach();
    std::thread(app::workers::nade_helper).detach();
    std::thread(app::workers::seed_trigger).detach();

#if defined(VESTA_ENABLE_CONSOLE) && VESTA_ENABLE_CONSOLE
    std::thread(app::workers::watchdog).detach();
#endif

#if defined(VESTA_WINDOWED_PROFILE) && VESTA_WINDOWED_PROFILE
    std::thread([] {
        ::Sleep(90'000);
        app::context().overlay.request_shutdown();
    }).detach();
#endif

    int exit_code = EXIT_SUCCESS;

    // Launch UI Overlay context main execution block
    if (!app::context().overlay.launch())
    {
        app::context().diagnostics.error("Overlay startup failed: ImGui runtime windows layout initialization broke during setup loop.");
        exit_code = EXIT_FAILURE;
    }

    // Safe ordered stack resource destruction path
    scripting::runtime().shutdown();
    simulation::trace_session::shutdown();
    simulation::shot_trace::shutdown();

    return exit_code;
}
