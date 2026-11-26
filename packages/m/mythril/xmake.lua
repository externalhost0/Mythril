package("mythril")
    set_homepage("https://github.com/externalhost0/Mythril")
    set_description("Bindless Vulkan renderer with a render graph")

    -- this package lives in the library repo, so the checked out repo is the source
    set_sourcedir(path.join(os.scriptdir(), "..", "..", ".."))

    add_configs("imgui",              {description = "Enable the ImGui plugin (standard branch).", default = false, type = "boolean"})
    add_configs("imgui_docking",      {description = "Enable the ImGui plugin (docking branch).", default = false, type = "boolean"})
    add_configs("tracy",              {description = "Enable Tracy profiling.", default = false, type = "boolean"})
    add_configs("tracy_gpu",          {description = "Enable Tracy GPU profiling (implies tracy).", default = false, type = "boolean"})
    add_configs("tracy_gpu_commands", {description = "Enable Tracy per-command GPU zones (implies tracy_gpu).", default = false, type = "boolean"})
    add_configs("shared",             {description = "Build shared library.", default = false, type = "boolean", readonly = true})

    add_deps("cmake", {kind = "binary"})
    add_deps("vulkan-headers")

    on_load(function (package)
        local imgui, docking = package:config("imgui"), package:config("imgui_docking")
        local tracy_commands = package:config("tracy_gpu_commands")
        local tracy_gpu = package:config("tracy_gpu") or tracy_commands
        local tracy = package:config("tracy") or tracy_gpu
        if imgui and docking then
            raise("mythril: enable only one of imgui and imgui_docking")
        end

        -- link order matters for static libraries
        package:add("links", "mythril", "mythril-spirv", "volk", "fmt")
        package:add("defines", "VK_NO_PROTOTYPES", "VK_ENABLE_BETA_EXTENSIONS")
        if imgui or docking then
            package:add("links", "imgui")
            package:add("defines", "MYTH_ENABLED_IMGUI", "IMGUI_IMPL_VULKAN_USE_VOLK", "IMGUI_DISABLE_OBSOLETE_FUNCTIONS")
            if docking then
                package:add("defines", "MYTH_ENABLED_IMGUI_DOCKING")
            end
        end
        if tracy then
            package:add("links", "TracyClient")
            package:add("defines", "MYTH_ENABLED_TRACY", "TRACY_ENABLE")
            if package:is_plat("linux") then
                package:add("syslinks", "pthread", "dl")
            elseif package:is_plat("windows", "mingw") then
                package:add("syslinks", "ws2_32", "dbghelp", "advapi32", "user32")
            end
        end
        if tracy_gpu then
            package:add("defines", "MYTH_ENABLED_TRACY_GPU", "TRACY_VK_USE_SYMBOL_TABLE")
            if package:is_plat("macosx") then
                package:add("defines", "TRACY_VK_QUERY_COUNT=4096")
            elseif tracy_commands then
                package:add("defines", "MYTH_ENABLED_TRACY_GPU_COMMANDS")
            end
        end
        if package:is_plat("macosx") then
            package:add("defines", "_LIBCPP_REMOVE_TRANSITIVE_INCLUDES")
        end
    end)

    on_install("windows", "linux", "macosx", function (package)
        local tracy_commands = package:config("tracy_gpu_commands")
        local tracy_gpu = package:config("tracy_gpu") or tracy_commands
        local tracy = package:config("tracy") or tracy_gpu
        local function onoff(value) return value and "ON" or "OFF" end
        local configs = {
            "-DMYTH_BUILD_SAMPLES=OFF",
            "-DMYTH_BUILD_TESTS=OFF",
            "-DMYTH_INSTALL=ON",
            -- fmt's debug "d" postfix would change the library name the package links
            "-DFMT_DEBUG_POSTFIX=",
            "-DMYTH_ENABLE_IMGUI_STANDARD=" .. onoff(package:config("imgui")),
            "-DMYTH_ENABLE_IMGUI_DOCKING=" .. onoff(package:config("imgui_docking")),
            "-DMYTH_ENABLE_TRACY=" .. onoff(tracy),
            "-DMYTH_ENABLE_TRACY_GPU=" .. onoff(tracy_gpu),
            "-DMYTH_ENABLE_TRACY_GPU_COMMANDS=" .. onoff(tracy_commands),
        }
        import("package.tools.cmake").install(package, configs, {targets = {"mythril", "mythril-reflect"}})
    end)

    on_test(function (package)
        assert(package:has_cxxincludes("mythril/CTX.h", {configs = {languages = "c++20"}}))
    end)
