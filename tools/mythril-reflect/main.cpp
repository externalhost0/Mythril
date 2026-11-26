#include "PushLayout.h"
#include "ShaderReflection.h"
#include "SlangCompiler.h"
#include "SpirvLayout.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {
	using namespace mythril;

	struct Options {
		std::filesystem::path outDir;
		std::filesystem::path spirvDir;
		std::filesystem::path depfile;
		std::filesystem::path stamp;
		HeaderOptions header;
		std::vector<std::filesystem::path> searchPaths;
		struct Source {
			std::filesystem::path path;
			bool typed;
		};
		std::vector<Source> sources;
		std::vector<std::filesystem::path> modules;
		bool debug = false;
	};

	template<class... Args>
	bool Fail(fmt::format_string<Args...> format, Args&&... args) {
		fmt::print(stderr, "mythril-reflect: error: {}\n", fmt::format(format, std::forward<Args>(args)...));
		return false;
	}

	bool ParseArgs(const int argc, char** argv, Options& options) {
		for (int i = 1; i < argc; ++i) {
			const std::string_view arg = argv[i];
			const auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
			const char* next = nullptr;
			if (arg == "--out-dir" && (next = value()))
				options.outDir = next;
			else if (arg == "--spirv-dir" && (next = value()))
				options.spirvDir = next;
			else if (arg == "--depfile" && (next = value()))
				options.depfile = next;
			else if (arg == "--stamp" && (next = value()))
				options.stamp = next;
			else if (arg == "--types-header" && (next = value()))
				options.header.typesHeader = next;
			else if (arg == "--types-namespace" && (next = value()))
				options.header.typesNamespace = next;
			else if (arg == "--namespace" && (next = value()))
				options.header.ns = next;
			else if (arg == "--search-path" && (next = value()))
				options.searchPaths.emplace_back(next);
			else if (arg == "--matrix-layout" && (next = value())) {
				const std::string_view mode = next;
				if (mode != "row" && mode != "column")
					return false;
				options.header.matrixMode = mode == "row" ? SLANG_MATRIX_LAYOUT_ROW_MAJOR : SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
			} else if (arg == "--module" && (next = value()))
				options.modules.push_back(std::filesystem::weakly_canonical(next));
			else if (arg == "--debug")
				options.debug = true;
			else if (arg == "--untyped" && (next = value()))
				options.sources.push_back({next, false});
			else if (!arg.starts_with("--"))
				options.sources.push_back({std::filesystem::path(arg), true});
			else
				return false;
		}
		return !options.outDir.empty() && !options.sources.empty();
	}

	std::string DepfilePath(const std::filesystem::path& path) {
		std::string out;
		for (const char c: path.generic_string()) {
			if (c == ' ' || c == '#' || c == '\\')
				out += '\\';
			out += c == '$' ? "$$" : std::string(1, c);
		}
		return out;
	}

	bool WriteFile(const std::filesystem::path& path, const std::string& text) {
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file << text;
		return static_cast<bool>(file);
	}

	bool WriteFileAtomicallyIfChanged(const std::filesystem::path& path, const std::string& text) {
		if (std::ifstream old(path, std::ios::binary); old && std::string(std::istreambuf_iterator<char>(old), {}) == text)
			return true;
		std::filesystem::path temp = path;
		temp += ".tmp";
		if (!WriteFile(temp, text))
			return false;
		std::error_code error;
		std::filesystem::rename(temp, path, error);
		return !error;
	}
	struct Compiled {
		std::string name;
		bool typed;
		std::filesystem::path source;
		ReflectedTypes types;
		std::vector<uint32_t> spirv;
		std::vector<uint32_t> threadGroupSize;
	};

	bool IsIdentifier(const std::string_view name) {
		return !name.empty() && !std::isdigit(static_cast<unsigned char>(name[0])) && std::ranges::all_of(name, [](const char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
	}

	std::string Stem(const std::filesystem::path& path) {
		const std::string name = path.filename().string();
		return name.substr(0, name.find('.'));
	}

	bool CompileShader(SlangCompiler& compiler, const Options::Source& input, const HeaderOptions& options, StructRegistry& structs, PushLayouts& pushLayouts, Compiled& shader) {
		const std::filesystem::path& source = input.path;
		if (!std::filesystem::exists(source))
			return Fail("shader '{}' does not exist", source.string());
		if (!IsIdentifier(Stem(source)))
			return Fail("shader '{}' needs a file name that is a C++ identifier, its data goes in namespace {}::<name>", source.string(), options.ns);
		std::string diagnostics;
		Slang::ComPtr<slang::IComponentType> linked = compiler.linkFromDisk(source, diagnostics);
		if (!linked)
			return Fail("{}", diagnostics);
		slang::ProgramLayout* layout = linked->getLayout(0);
		Slang::ComPtr<slang::IBlob> spirv;
		Slang::ComPtr<slang::IBlob> codeDiagnostics;
		if (!layout || SLANG_FAILED(linked->getTargetCode(0, spirv.writeRef(), codeDiagnostics.writeRef())) || !spirv)
			return Fail("shader '{}' produced no layout or SPIR-V:\n{}", source.string(), codeDiagnostics ? static_cast<const char*>(codeDiagnostics->getBufferPointer()) : "");
		if (codeDiagnostics)
			diagnostics.append(static_cast<const char*>(codeDiagnostics->getBufferPointer()), codeDiagnostics->getBufferSize());
		if (const size_t end = diagnostics.find_last_not_of(std::string_view("\0\n\r\t ", 5)); end != std::string::npos)
			fmt::print(stderr, "{}: warning: {}\n", source.string(), diagnostics.substr(0, end + 1));

		shader = {.name = source.filename().string(), .typed = input.typed, .source = std::filesystem::weakly_canonical(source)};
		for (SlangUInt i = 0; i < layout->getEntryPointCount(); ++i) {
			if (layout->getEntryPointByIndex(i)->getStage() != SLANG_STAGE_COMPUTE)
				continue;
			SlangUInt size[3] = {};
			layout->getEntryPointByIndex(i)->getComputeThreadGroupSize(3, size);
			shader.threadGroupSize.assign(std::begin(size), std::end(size));
		}
		shader.spirv.resize(spirv->getBufferSize() / sizeof(uint32_t));
		std::memcpy(shader.spirv.data(), spirv->getBufferPointer(), shader.spirv.size() * sizeof(uint32_t));
		const SpirvLayout spirvLayout(shader.spirv);
		std::vector<std::string> errors;
		if (std::string error = CheckSPIRVRules(spirvLayout); !error.empty())
			errors.push_back(std::move(error));
		else if (input.typed) {
			ReflectPushTypes(layout, source.string(), structs, shader.types);
			PushLayouts shaderLayouts;
			errors = CheckHeader(shader.types, spirvLayout, options, shaderLayouts);
			for (auto& [structName, pushLayout]: shaderLayouts)
				if (const auto [it, inserted] = pushLayouts.try_emplace(structName, pushLayout); errors.empty() && !inserted && it->second.hash != pushLayout.hash)
					return Fail("push block '{}' has a different SPIR-V layout in '{}' than in an earlier shader:\n  {}\n  {}", structName, source.string(), it->second.description, pushLayout.description);
		}
		for (const std::string& error: errors)
			fmt::print(stderr, "{}: error: {}\n", source.string(), error);
		return errors.empty();
	}

	// a struct declared in a module goes in that module's header, and every header that uses it includes that one
	bool AssignOwners(const SlangCompiler::Declarations& declarations, const std::vector<Compiled>& compiled, const StructRegistry& structs, std::map<std::string, std::filesystem::path>& ownerByStruct) {
		for (const auto& [structName, file]: declarations.structs) {
			if (!structs.contains(structName))
				continue;
			const std::filesystem::path owner = std::filesystem::weakly_canonical(file);
			if (std::ranges::any_of(compiled, [&](const Compiled& shader) { return !shader.typed && shader.source == owner; }))
				continue;
			if (const auto [it, inserted] = ownerByStruct.try_emplace(structName, owner); !inserted && it->second != owner)
				return Fail("struct '{}' is declared in '{}' and '{}', and each generated struct is defined once. Move it into one module that both import, or rename one.", structName, it->second.string(), owner.string());
		}
		return true;
	}

	struct Header {
		std::string sourceName;
		const Compiled* shader = nullptr;
		ReflectedTypes types;
		std::vector<std::string> includes;
	};

	std::string HeaderName(const std::filesystem::path& path) { return Stem(path) + ".shadergen.h"; }

	bool PlanHeaders(const std::vector<Compiled>& compiled, const std::vector<std::filesystem::path>& modules, const std::map<std::string, std::filesystem::path>& ownerByStruct, std::map<std::filesystem::path, Header>& headers) {
		for (const Compiled& shader: compiled)
			headers[shader.source] = {.sourceName = shader.name, .shader = &shader};
		for (const std::filesystem::path& module: modules)
			headers.try_emplace(module, Header{.sourceName = module.filename().string()});
		for (const auto& [structName, owner]: ownerByStruct)
			if (!headers.contains(owner))
				return Fail("'{}' declares the generated struct '{}', add it to MODULES in mythril_reflect_shaders so its {} is a build output", owner.string(), structName, HeaderName(owner));
		const auto include = [](Header& header, const std::filesystem::path& owner) {
			if (std::string name = HeaderName(owner); std::ranges::find(header.includes, name) == header.includes.end())
				header.includes.push_back(std::move(name));
		};
		for (const Compiled& shader: compiled) {
			for (const ReflectedStruct& type: shader.types.structs) {
				const auto it = ownerByStruct.find(type.name);
				const std::filesystem::path& owner = it == ownerByStruct.end() ? shader.source : it->second;
				Header& header = headers[owner];
				if (!header.types.find(type.name))
					header.types.structs.push_back(type);
				if (owner != shader.source)
					include(headers[shader.source], owner);
			}
		}
		for (auto& [owner, header]: headers)
			for (const ReflectedStruct& type: header.types.structs)
				for (const ReflectedField& field: type.fields)
					if (const auto it = ownerByStruct.find(field.type.name); field.type.kind == ReflectedType::Kind::Struct && it != ownerByStruct.end() && it->second != owner)
						include(header, it->second);
		return true;
	}

	using Outputs = std::vector<std::pair<std::filesystem::path, std::string>>;

	bool RenderOutputs(const std::map<std::filesystem::path, Header>& headers, const std::vector<SlangCompiler::Constant>& constants, const PushLayouts& pushLayouts, const Options& options, Outputs& outputs, Outputs& spirvFiles) {
		const std::filesystem::path& spirvDir = options.spirvDir.empty() ? options.outDir : options.spirvDir;
		std::map<std::string, std::filesystem::path> ownerByHeader;
		for (const auto& [owner, header]: headers) {
			const std::string name = HeaderName(owner);
			if (const auto [it, inserted] = ownerByHeader.try_emplace(name, owner); !inserted)
				return Fail("'{}' and '{}' would both generate {}, rename one", it->second.string(), owner.string(), name);
			const bool typed = !header.types.structs.empty() || !header.includes.empty();
			std::string text = typed ? EmitHeader(header.sourceName, header.types, header.includes, pushLayouts, options.header) : HeaderBanner(header.sourceName);
			ModuleData data = {.name = Stem(header.sourceName)};
			for (const SlangCompiler::Constant& constant: constants)
				if (std::filesystem::weakly_canonical(constant.file) == owner)
					data.constants.push_back(constant);
			if (const Compiled* shader = header.shader) {
				data.spirvFile = shader->name + ".spv";
				data.threadGroupSize = shader->threadGroupSize;
				spirvFiles.emplace_back(spirvDir / data.spirvFile, std::string(reinterpret_cast<const char*>(shader->spirv.data()), shader->spirv.size() * sizeof(uint32_t)));
			}
			text += EmitModuleData(data, options.header);
			outputs.emplace_back(options.outDir / name, std::move(text));
		}
		return true;
	}

	bool WriteOutputs(const Options& options, const Outputs& outputs, const Outputs& spirvFiles, const std::vector<std::filesystem::path>& dependencies) {
		std::filesystem::create_directories(options.outDir);
		std::filesystem::create_directories(options.spirvDir.empty() ? options.outDir : options.spirvDir);
		for (const Outputs* files: {&outputs, &spirvFiles})
			for (const auto& [path, text]: *files)
				if (!WriteFileAtomicallyIfChanged(path, text))
					return Fail("cannot write '{}'", path.string());

		if (!options.depfile.empty()) {
			std::string depfile = options.stamp.empty() ? "" : DepfilePath(std::filesystem::absolute(options.stamp)) + " ";
			for (const auto& [path, text]: outputs)
				depfile += DepfilePath(std::filesystem::absolute(path)) + " ";
			depfile.back() = ':';
			for (const std::filesystem::path& dependency: dependencies)
				if (std::filesystem::exists(dependency))
					depfile += " " + DepfilePath(std::filesystem::absolute(dependency));
			if (!WriteFile(options.depfile, depfile + "\n"))
				return Fail("cannot write '{}'", options.depfile.string());
		}
		if (!options.stamp.empty() && !WriteFile(options.stamp, ""))
			return Fail("cannot write '{}'", options.stamp.string());
		return true;
	}

	bool Run(const int argc, char** argv) {
		Options options;
		if (!ParseArgs(argc, argv, options))
			return Fail("usage: mythril-reflect --out-dir <dir> [--spirv-dir <dir>] [--depfile <file>] [--stamp <file>] [--types-header <header>] [--types-namespace <ns>] [--namespace <ns>] [--matrix-layout row|column] [--debug] [--search-path <dir>]... [--module <module.slang>]... [--untyped <shader.slang>]... <shader.slang>...");
		std::string diagnostics;
		std::optional<SlangCompiler> compiler = SlangCompiler::Create({.searchPaths = options.searchPaths, .matrixLayout = options.header.matrixMode, .debug = options.debug}, diagnostics);
		if (!compiler)
			return Fail("{}", diagnostics);

		std::vector<Compiled> compiled(options.sources.size());
		StructRegistry structs;
		PushLayouts pushLayouts;
		for (size_t i = 0; i < options.sources.size(); ++i)
			if (!CompileShader(*compiler, options.sources[i], options.header, structs, pushLayouts, compiled[i]))
				return false;

		const SlangCompiler::Declarations declarations = compiler->loadedDeclarations();
		std::map<std::string, std::filesystem::path> ownerByStruct;
		std::map<std::filesystem::path, Header> headers;
		Outputs outputs;
		Outputs spirvFiles;
		return AssignOwners(declarations, compiled, structs, ownerByStruct) && PlanHeaders(compiled, options.modules, ownerByStruct, headers) &&
		       RenderOutputs(headers, declarations.constants, pushLayouts, options, outputs, spirvFiles) && WriteOutputs(options, outputs, spirvFiles, compiler->loadedDependencyFiles());
	}
} // namespace

int main(const int argc, char** argv) { return Run(argc, argv) ? 0 : 1; }
