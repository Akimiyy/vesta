#pragma once

#include <core/input/input.hpp>
#include <core/memory/addresses.hpp>
#include <core/memory/modules.hpp>
#include <core/memory/process.hpp>
#include <render/menu/menu.hpp>
#include <render/overlay/overlay.hpp>
#include <system/diagnostics.hpp>

namespace app {

    struct context_t
    {
        // Delete copy and move constructors/assignments to ensure context is never duplicated or shifted in memory
        context_t() = default;
        ~context_t() = default;
        
        context_t(const context_t&) = delete;
        context_t& operator=(const context_t&) = delete;
        context_t(context_t&&) = delete;
        context_t& operator=(context_t&&) = delete;

        // Core Platform / OS Subsystems
        platform::windows::diagnostic_sink diagnostics{};
        platform::windows::input_gateway    input{};
        platform::windows::process_session  process{};
        platform::windows::module_catalog   modules{};

        // Game Memory Subsystems
        game::address_catalog               addresses{};

        // Graphical Interface Subsystems
        overlay_t                           overlay{};
        menu_t                              menu{};
    };

    /**
     * @brief Retrieves the thread-safe global application context singleton instance.
     * @return A reference to the persistent context configuration matrix.
     */
    [[nodiscard]] inline context_t& context() noexcept
    {
        static context_t instance{};
        return instance;
    }

} // namespace app
