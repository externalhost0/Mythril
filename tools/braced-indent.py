#!/usr/bin/env python3
# runs clang-format on each file, then block indents braced lists that follow another argument, like `f(a, {`.
# clang-format always aligns those under the brace and has no option to stop it.
# usage: braced-indent.py [--check] <clang-format> <files...>
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

OPENER = re.compile(r',\s*(?:[\w:]+(?:<[^<>]*>)?)?\{$')


def scan(text):
    # returns code braces as (line, col, char) and the lines that start inside a string literal
    braces, in_string_lines = [], set()
    i, line, col, n = 0, 0, 0, len(text)
    state, raw_end = None, ''

    def advance(count):
        nonlocal i, line, col
        for _ in range(count):
            if text[i] == '\n':
                line += 1
                col = 0
                if state in ('raw', 'str'):
                    in_string_lines.add(line)
            else:
                col += 1
            i += 1

    while i < n:
        c = text[i]
        if state is None:
            if text.startswith('//', i):
                end = text.find('\n', i)
                advance((end if end != -1 else n) - i)
                continue
            if text.startswith('/*', i):
                state = 'block'
                advance(2)
                continue
            raw = re.match(r'(?:u8|u|U|L)?R"([^(\s]*)\(', text[i:i + 24])
            if raw and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == '_')):
                state, raw_end = 'raw', ')' + raw.group(1) + '"'
                advance(raw.end())
                continue
            if c == '"':
                state = 'str'
                advance(1)
                continue
            if c == "'" and not (i > 0 and text[i - 1].isalnum()):
                state = 'char'
                advance(1)
                continue
            if c in '{}':
                braces.append((line, col, c))
            advance(1)
        elif state == 'block':
            if text.startswith('*/', i):
                state = None
                advance(2)
            else:
                advance(1)
        elif state == 'raw':
            if text.startswith(raw_end, i):
                state = None
                advance(len(raw_end))
            else:
                advance(1)
        else:
            quote = '"' if state == 'str' else "'"
            if c == '\\':
                advance(2)
            elif c == quote or c == '\n':
                state = None
                advance(1)
            else:
                advance(1)
    return braces, in_string_lines


def indent(s):
    return len(s) - len(s.lstrip(' '))


def fix_once(lines, start):
    text = '\n'.join(lines)
    braces, in_string_lines = scan(text)
    match, stack = {}, []
    for b in braces:
        if b[2] == '{':
            stack.append(b)
        elif stack:
            match[stack.pop()[:2]] = b[:2]
    for (open_line, open_col), (close_line, close_col) in sorted(match.items()):
        if open_line < start or close_line == open_line:
            continue
        head = lines[open_line]
        if open_col != len(head.rstrip()) - 1 or not OPENER.search(head[:open_col + 1].rstrip()):
            continue
        closer = lines[close_line]
        if close_col != indent(closer):
            continue
        shift = close_col - indent(head)
        body = range(open_line + 1, close_line + 1)
        if shift <= 0 or any(l in in_string_lines for l in body):
            continue
        if any(lines[l].strip() and indent(lines[l]) < shift for l in body):
            continue
        for l in body:
            lines[l] = lines[l][shift:] if lines[l].strip() else lines[l]
        return open_line + 1
    return None


def fix(text):
    lines = text.split('\n')
    start = 0
    while (start := fix_once(lines, start)) is not None:
        pass
    return '\n'.join(lines)


def process(fmt, path, check):
    with open(path, encoding='utf-8') as f:
        original = f.read()
    formatted = subprocess.run([fmt, '--style=file', f'--assume-filename={path}'], input=original, capture_output=True, text=True, check=True).stdout
    fixed = fix(formatted)
    if fixed == original:
        return None
    if not check:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(fixed)
    return path


def main():
    args = sys.argv[1:]
    check = bool(args) and args[0] == '--check'
    if check:
        args = args[1:]
    fmt, files = args[0], args[1:]
    with ThreadPoolExecutor() as pool:
        dirty = [p for p in pool.map(lambda p: process(fmt, p, check), files) if p]
    for p in dirty:
        print(p)
    return 1 if dirty and check else 0


if __name__ == '__main__':
    sys.exit(main())
