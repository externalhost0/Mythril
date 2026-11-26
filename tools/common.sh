# shared helpers for tools/clang-*.sh, meant to be sourced. callers set `name` to their script name.

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# find_tool <binary> <override>: the override if set, else PATH, else homebrew llvm
find_tool() {
	local bin=$1 override=$2 found=${2:-$(command -v "$1" || true)}
	if [[ -z $found && -x /opt/homebrew/opt/llvm/bin/$bin ]]; then
		found=/opt/homebrew/opt/llvm/bin/$bin
	fi
	if [[ ! -x $found ]]; then
		echo "$name: $bin not found, set $(echo "$bin" | tr 'a-z-' 'A-Z_')" >&2
		exit 1
	fi
	echo "$found"
}

# collect_files [files...]: sets the global array files to the given files, else every tracked c++ source
collect_files() {
	if [[ $# -gt 0 ]]; then
		files=("$@")
		return
	fi
	files=()
	local f
	while IFS= read -r f; do
		files+=("$root/$f")
	done < <(git -C "$root" ls-files -- lib include samples tests tools |
		grep -E '\.(cpp|cc|cxx|h|hpp)$' | grep -vE '^tests/push_layout/golden/' || true)
}

report_clean() { echo "$name: nothing to fix ($1 files checked)"; }
report_no_files() { echo "$name: no files to check"; }
