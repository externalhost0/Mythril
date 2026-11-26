#!/usr/bin/env bash
# formats the given files in place with clang-format, or every tracked c++ source when none are given.
# --check only lists files that need formatting and exits 1 if any do.
# env: CLANG_FORMAT (binary).
set -euo pipefail

name=clang-format.sh
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

if ! output=$("$fmt" --style=file --dry-run "${files[@]}" 2>&1); then
	echo "$output" >&2
	exit 1
fi
dirty=$(echo "$output" | grep -oE '^[^ :]+:[0-9]+:[0-9]+: (warning|error)' | cut -d: -f1 | sort -u || true)

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

echo "$dirty" | xargs "$fmt" --style=file -i
echo "$name: formatted $count of ${#files[@]} files:"
echo "$dirty"
