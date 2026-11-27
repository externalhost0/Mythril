# shared helpers for tools/format.sh and tools/clang-tidy.sh, meant to be sourced. callers set `name` to their script name.

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# find_tool <binary> <override>: the override if set, else PATH, else homebrew llvm
find_tool() {
	local bin=$1 found=${2:-$(command -v "$1" || true)}
	if [[ -z $found && -x /opt/homebrew/opt/llvm/bin/$bin ]]; then
		found=/opt/homebrew/opt/llvm/bin/$bin
	fi
	if [[ ! -x $found ]]; then
		echo "$name: $bin not found, set $(echo "$bin" | tr 'a-z-' 'A-Z_')" >&2
		exit 1
	fi
	echo "$found"
}

source_include='^(lib|include|samples|tests|tools)/.*\.(cpp|cc|cxx|h|hpp)$'
source_exclude='^(tests/push_layout/golden|build|\.cache)/'

# collect_files [files...]: sets the global array files (absolute paths) to the given files, else every tracked file, keeping only c++ sources the tools own
collect_files() {
	local candidates=() f abs rel
	if [[ $# -gt 0 ]]; then
		candidates=("$@")
	else
		while IFS= read -r f; do
			candidates+=("$root/$f")
		done < <(git -C "$root" ls-files)
	fi
	files=()
	for f in ${candidates[@]+"${candidates[@]}"}; do
		[[ -f $f ]] || continue
		abs=$(cd "$(dirname "$f")" && pwd)/$(basename "$f")
		rel=${abs#"$root"/}
		if [[ $rel =~ $source_include && ! $rel =~ $source_exclude ]]; then
			files+=("$abs")
		fi
	done
}

report_clean() { echo "$name: nothing to fix ($1 files checked)"; }
report_no_files() { echo "$name: no files to check"; }
