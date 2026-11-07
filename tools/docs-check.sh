#!/usr/bin/env bash
#
# docs-check.sh - documentation integrity check for the repository.
#
# Registered as the CTest test `docs-links-check` (see CMakeLists.txt) and run
# from the repository root. With no dependency beyond bash, git and the tools
# the build already requires, it enforces:
#
#   * every relative markdown link [text](path) points at a file that exists;
#   * every anchor citation (a documentation file path followed by a '#slug',
#     in markdown or in a source comment) resolves to a heading in that file,
#     using GitHub slug rules (lower-case, non-alphanumerics to '-');
#   * no markdown file has trailing whitespace or a heading that is not followed
#     by a blank line.
#
# Each offender is printed as `file:line: message`, and the exit status is
# non-zero when at least one offender was found.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "docs-check: not inside a git work tree; run from the repository root" >&2
    exit 2
fi

problems=0

report() { # report <file> <line> <message>
    printf '%s:%s: %s\n' "$1" "$2" "$3"
    problems=$((problems + 1))
}

# GitHub heading slug: lower-case, every run of non-alphanumerics becomes one
# hyphen, and leading/trailing hyphens are dropped (digits are kept).
slugify() {
    printf '%s' "$1" \
        | tr '[:upper:]' '[:lower:]' \
        | sed -E 's/[^a-z0-9]+/-/g; s/^-+//; s/-+$//'
}

# Succeeds when <anchor> is the slug of a heading in <markdown file>.
anchor_exists() { # anchor_exists <file> <anchor>
    local file="$1" anchor="$2" heading
    while IFS= read -r heading; do
        if [ "$(slugify "$heading")" = "$anchor" ]; then
            return 0
        fi
    done < <(grep -E '^#{1,6} ' "$file" | sed -E 's/^#{1,6} +//' || true)
    return 1
}

# --- markdown relative links ------------------------------------------------

check_links() { # check_links <file>
    local file="$1" lineno=0 in_fence=0 raw match target path

    while IFS= read -r raw || [ -n "$raw" ]; do
        lineno=$((lineno + 1))

        case "$raw" in
            '```'*|'~~~'*)
                if [ "$in_fence" -eq 0 ]; then in_fence=1; else in_fence=0; fi
                continue
                ;;
        esac
        if [ "$in_fence" -eq 1 ]; then
            continue
        fi

        while IFS= read -r match; do
            if [ -z "$match" ]; then
                continue
            fi
            target="${match#*](}"
            target="${target%)}"
            target="${target%% *}"          # drop an optional link title
            if [ -z "$target" ] || [ "${target:0:1}" = '#' ]; then
                continue
            fi
            case "$target" in
                http://*|https://*|mailto:*) continue ;;
            esac
            path="${target%%#*}"            # strip a trailing anchor
            if [ -z "$path" ]; then
                continue
            fi
            if [ ! -e "$path" ]; then
                report "$file" "$lineno" "missing link target: $target"
            fi
        done < <(grep -oE '\]\([^)]+\)' <<<"$raw" || true)
    done < "$file"
}

# --- anchor citations in markdown and source comments -----------------------

check_citations() { # check_citations <file>
    local file="$1" line match mdpath anchor

    while IFS=: read -r line match; do
        if [ -z "$match" ]; then
            continue
        fi
        mdpath="${match%%#*}"
        anchor="${match#*#}"
        if [ ! -f "$mdpath" ]; then
            report "$file" "$line" "citation names a missing file: $mdpath"
        elif ! anchor_exists "$mdpath" "$anchor"; then
            report "$file" "$line" "unknown anchor '#$anchor' in $mdpath"
        fi
    done < <(grep -noE 'docs/[A-Za-z0-9_./-]+\.md#[A-Za-z0-9_-]+' "$file" || true)
}

# --- markdown hygiene -------------------------------------------------------

check_hygiene() { # check_hygiene <file>
    local file="$1" lineno=0 heading_line=0 raw

    while IFS= read -r raw || [ -n "$raw" ]; do
        lineno=$((lineno + 1))

        if [ "$heading_line" -ne 0 ]; then
            if [ -n "$raw" ]; then
                report "$file" "$heading_line" "heading not followed by a blank line"
            fi
            heading_line=0
        fi

        if [[ "$raw" =~ [[:space:]]$ ]]; then
            report "$file" "$lineno" "trailing whitespace"
        fi

        if [[ "$raw" =~ ^#{1,6}[[:space:]] ]]; then
            heading_line=$lineno
        fi
    done < "$file"
}

mapfile -t markdown_files < <(git ls-files '*.md')
mapfile -t citation_files < <(git ls-files '*.md' '*.hpp' '*.cpp' '*.h' '*.c' '*.sh' '*.cmake' 'CMakeLists.txt')

for file in "${markdown_files[@]}"; do
    check_links "$file"
    check_hygiene "$file"
done

for file in "${citation_files[@]}"; do
    check_citations "$file"
done

if [ "$problems" -ne 0 ]; then
    echo "docs-check: $problems problem(s) found" >&2
    exit 1
fi

echo "docs-check: ok (${#markdown_files[@]} markdown files)"
