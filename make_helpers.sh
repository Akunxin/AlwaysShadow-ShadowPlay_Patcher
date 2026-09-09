#! /bin/bash

confirm() {
    local confirm
    read -ep "$1 " confirm # Add a space at the end.
    [[ "$confirm" == *([[:space:]])[yY]* ]]
}

write_if_diff() {
    local tmp="$(mktemp)"
    cat > "$tmp"

    # cksum is part of coreutils, already required by the build. No diffutils dependency.
    if [[ ! -f "$1" ]] || [[ "$(cksum < "$tmp")" != "$(cksum < "$1")" ]]; then
        cp -- "$tmp" "$1"
    fi

    rm -- "$tmp"
}

latest_release() {
    # gh release view makes it easier to obtain the latest release, but it doesn't show draft releases.
    gh release list --json "$1" --jq ".[0].$1"
}

known_tags() {
    local known
    if command -v gh > /dev/null 2>&1 &&
        known="$(gh release list --json tagName --jq '.[].tagName' 2> /dev/null)" && [[ -n "$known" ]]; then
        printf '%s\n' "$known"
    elif [[ -s "$1" ]]; then
        cat -- "$1"
    else
        known="$(git tag --list 2> /dev/null)"
        printf '%s\n' "${known:-local}"
    fi
}

# User runs "make_utils.sh <func name> <func args>", and we run the function with the args.
"$@"
