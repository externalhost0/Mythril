-- compiles and reflects Slang shaders with mythril-reflect, the xmake form of mythril_reflect_shaders
--
--   add_rules("@mythril/shaders")
--   add_files("shaders/*.slang")                          -- typed, generates push structs
--   add_files("shaders/lib.slang", {module = true})       -- imported module, generates <module>.shadergen.h
--   add_files("shaders/raw.slang", {untyped = true})      -- compiled only
--   set_values("mythril.shaders.matrix_layout", "column") -- or "row" (default)
--   set_values("mythril.shaders.spirv_dir", "build/data") -- default is the target directory
--   set_values("mythril.shaders.search_paths", "shaders") -- extra import paths, relative to the project
--   set_values("mythril.shaders.types_header", "shader_types.h")
--   set_values("mythril.shaders.types_namespace", "mythril::GPU")
--   set_values("mythril.shaders.namespace", "my_ns")
--
-- the generated <shader>.shadergen.h files are on the target's include path
rule("shaders")
    set_extensions(".slang")

    on_config(function (target)
        target:add("includedirs", path.join(target:autogendir(), "rules", "mythril"), {public = false})
    end)

    before_buildcmd_files(function (target, batchcmds, sourcebatch, opt)
        local pkg = target:pkg("mythril")
        assert(pkg, "target '%s' uses @mythril/shaders, so it also needs add_packages(\"mythril\")", target:name())
        local tool = path.join(pkg:installdir(), "bin", "mythril-reflect" .. (is_host("windows") and ".exe" or ""))
        assert(os.isfile(tool), "mythril-reflect not found at %s", tool)

        local outdir = path.join(target:autogendir(), "rules", "mythril")
        local stamp = path.join(outdir, "reflect.stamp")
        local depfile = path.join(outdir, "reflect.d")
        local function value(name)
            return target:values("mythril.shaders." .. name)
        end

        local args = {"--out-dir", outdir, "--depfile", depfile, "--stamp", stamp}
        if is_mode("debug") then
            table.insert(args, "--debug")
        end
        local spirvdir = value("spirv_dir")
        if spirvdir then
            spirvdir = path.absolute(spirvdir)
        else
            spirvdir = target:targetdir()
        end
        table.join2(args, {"--spirv-dir", spirvdir})
        for _, option in ipairs({{"matrix_layout", "--matrix-layout"}, {"types_header", "--types-header"}, {"types_namespace", "--types-namespace"}, {"namespace", "--namespace"}}) do
            local v = value(option[1])
            if v then
                table.join2(args, {option[2], v})
            end
        end

        local searchpaths = {}
        for _, dir in ipairs(table.wrap(target:values("mythril.shaders.search_paths"))) do
            table.insert(searchpaths, path.absolute(dir))
        end
        for _, file in ipairs(sourcebatch.sourcefiles) do
            table.insert(searchpaths, path.directory(path.absolute(file)))
        end
        local seen = {}
        for _, dir in ipairs(searchpaths) do
            if not seen[dir] then
                seen[dir] = true
                table.join2(args, {"--search-path", dir})
            end
        end

        local sources = {}
        for _, file in ipairs(sourcebatch.sourcefiles) do
            local conf = target:fileconfig(file) or {}
            local abs = path.absolute(file)
            if conf.module then
                table.join2(args, {"--module", abs})
            elseif conf.untyped then
                table.join2(args, {"--untyped", abs})
            else
                table.insert(sources, abs)
            end
            batchcmds:add_depfiles(abs)
        end
        table.join2(args, sources)

        -- imported modules the tool reported on the previous run
        if os.isfile(depfile) then
            local text = io.readfile(depfile):gsub("\\\n", " ")
            for dep in text:gsub("^[^:]*:", ""):gmatch("%S+") do
                if os.isfile(dep) then
                    batchcmds:add_depfiles(dep)
                end
            end
        end

        batchcmds:mkdir(outdir)
        batchcmds:show_progress(opt.progress, "${color.build.object}reflecting shaders for %s", target:name())
        batchcmds:vrunv(tool, args)
        batchcmds:add_depfiles(tool)
        batchcmds:set_depmtime(os.mtime(stamp))
        batchcmds:set_depcache(target:dependfile(stamp))
    end)
