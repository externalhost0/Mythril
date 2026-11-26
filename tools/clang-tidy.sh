#!/usr/bin/env bash
# runs clang-tidy on the given files against a preset's compile database.
# a header has no compile command, so it is checked through the translation units of its directory.
# env: MYTH_TIDY_PRESET (default debug-samples), MYTH_TIDY_JOBS (default 4), CLANG_TIDY (binary), VULKAN_SDK.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
preset=${MYTH_TIDY_PRESET:-debug-samples}
db=$root/build/$preset

if [[ ! -f $db/compile_commands.json ]]; then
	echo "clang-tidy.sh: missing $db/compile_commands.json, run: cmake --preset $preset" >&2
	exit 1
fi

tidy=${CLANG_TIDY:-$(command -v clang-tidy || true)}
if [[ -z $tidy && -x /opt/homebrew/opt/llvm/bin/clang-tidy ]]; then
	tidy=/opt/homebrew/opt/llvm/bin/clang-tidy
fi
if [[ -z $tidy ]]; then
	echo "clang-tidy.sh: clang-tidy not found, set CLANG_TIDY" >&2
	exit 1
fi

# a standalone clang-tidy does not know the apple sdk, libc++ or vulkan headers the real build finds
extra=()
if [[ $(uname) == Darwin ]]; then
	sdk=$(xcrun --show-sdk-path)
	extra+=("--extra-arg=-isysroot$sdk" "--extra-arg=-isystem$sdk/usr/include/c++/v1")
	if [[ -d /usr/local/include/vulkan ]]; then
		extra+=("--extra-arg=-isystem/usr/local/include")
	fi
fi
if [[ -n ${VULKAN_SDK:-} ]]; then
	extra+=("--extra-arg=-isystem$VULKAN_SDK/include")
fi

[[ $# -gt 0 ]] || exit 0

sources=()
headers=()
for f in "$@"; do
	abs=$(cd "$(dirname "$f")" && pwd)/$(basename "$f")
	case $f in
		*.h | *.hpp) headers+=("$abs") ;;
		*) sources+=("$abs") ;;
	esac
done

log=$(mktemp)
trap 'rm -f "$log"' EXIT
status=0

run() {
	local filter=$1
	shift
	printf '%s\0' "$@" | xargs -0 -n1 -P "${MYTH_TIDY_JOBS:-4}" \
		"$tidy" -p "$db" --quiet "--header-filter=$filter" ${extra[@]+"${extra[@]}"} >>"$log" 2>&1 || status=1
}

if [[ ${#sources[@]} -gt 0 ]]; then
	run "^$root/(include|lib|samples|tests|tools)/" "${sources[@]}"
fi

unit_dir() {
	local rel=${1#"$root"/}
	if [[ $rel == include/* || $rel == lib/* ]]; then
		echo "$root/lib/"
	else
		echo "$(dirname "$1")/"
	fi
}

units_in() {
	grep -o '"file": "[^"]*"' "$db/compile_commands.json" | sed 's/"file": "//;s/"$//' | grep "^$1" | grep -E '\.(cpp|cc|cxx)$' || true
}

if [[ ${#headers[@]} -gt 0 ]]; then
	dirs=$(for h in "${headers[@]}"; do unit_dir "$h"; done | sort -u)
	units=()
	for d in $dirs; do
		while IFS= read -r unit; do
			[[ -n $unit ]] && units+=("$unit")
		done < <(units_in "$d")
	done
	if [[ ${#units[@]} -gt 0 ]]; then
		pattern=$(printf '%s|' "${headers[@]}")
		run "^(${pattern%|})\$" "${units[@]}"
	fi
fi

# one header is reached from many units, print each diagnostic once
awk '/^[^ ].*: (warning|error): / { show = !seen[$0]++ } show' "$log"

if grep -q "shadergen.h' file not found" "$log"; then
	echo "clang-tidy.sh: generated shader headers are missing, run: cmake --build build/$preset" >&2
fi
exit $status
