# Sourced by the tools that start a game: sets PS_BUILTIN_PATCHES_DIR when it is unset, to the
# decanter checkout's folder holding the title's built-in patch file (psport does not ship it).
# An explicitly set PS_BUILTIN_PATCHES_DIR is never touched. Sets BUILTIN_PATCHES_NOTE to the line
# the caller prints once. Usage: resolve_builtin_patches ROOT PREFIX
resolve_builtin_patches() {
    local root=$1 prefix=$2 dir
    if [[ -n ${PS_BUILTIN_PATCHES_DIR:-} ]]; then
        BUILTIN_PATCHES_NOTE="$prefix: PS_BUILTIN_PATCHES_DIR=$PS_BUILTIN_PATCHES_DIR (set)"
        return
    fi
    local -a candidates=("$HOME/Projects/dev/decanter/crates/ps/data/bloodborne" "$root/../decanter/crates/ps/data/bloodborne")
    [[ -n ${PS_DECANTER_DIR:-} ]] && candidates=("$PS_DECANTER_DIR/crates/ps/data/bloodborne")
    for dir in "${candidates[@]}"; do
        if [[ -f $dir/Bloodborne.xml ]]; then
            export PS_BUILTIN_PATCHES_DIR=$(cd -- "$dir" && pwd)
            BUILTIN_PATCHES_NOTE="$prefix: PS_BUILTIN_PATCHES_DIR=$PS_BUILTIN_PATCHES_DIR (from the decanter checkout)"
            return
        fi
    done
    BUILTIN_PATCHES_NOTE="$prefix: PS_BUILTIN_PATCHES_DIR not set and no decanter checkout found: Bloodborne runs go without their built-in patches (set PS_DECANTER_DIR)"
}
