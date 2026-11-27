#!/usr/bin/env bash
# formats the given files in place with clang-format and braced-indent.py, or every tracked c++ source when none are given.
# --check only lists files that need formatting and exits 1 if any do.
# env: CLANG_FORMAT (binary).
set -euo pipefail

name=format.sh
source "$(dirname "$0")/common.sh"

check=0
if [[ ${1:-} == --check ]]; then
	check=1
	shift
fi

fmt=$(find_tool clang-format "${CLANG_FORMAT:-}")

collect_files "$@"
if [[ ${#files[@]} -eq 0 ]]; then
	report_no_files
	exit 0
fi

# clang-format plus a braced list indent pass it cannot express, see braced-indent.py
args=("$fmt")
[[ $check -eq 1 ]] && args=(--check "$fmt")
if ! dirty=$(python3 "$root/tools/braced-indent.py" "${args[@]}" "${files[@]}"); then
	[[ -n $dirty ]] || exit 1
fi

if [[ -z $dirty ]]; then
	report_clean ${#files[@]}
	exit 0
fi

count=$(echo "$dirty" | wc -l | tr -d ' ')
if [[ $check -eq 1 ]]; then
	echo "$name: $count of ${#files[@]} files need formatting:"
	echo "$dirty"
	exit 1
fi

echo "$name: formatted $count of ${#files[@]} files:"
echo "$dirty"
