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

# User runs "make_utils.sh <func name> <func args>", and we run the function with the args.
"$@"
