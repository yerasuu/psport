#!/usr/bin/env bash
# tools/nix_cache.sh STORE_PATH COMMIT TARGET: publishes STORE_PATH's closure (nix-build
# packaging/port.nix) as a flat Nix binary cache, for COMMIT. TARGET: github:OWNER/REPO, the assets
# of that repository's release "nix-cache" (gh, with GH_TOKEN), or a local directory with the same
# files. Only paths the target lacks are uploaded; then the commits older than the newest
# NIX_CACHE_KEEP (default 30) and the paths only they used are pruned.
# The closure is made content-addressed first: Nix accepts those paths without a signature, also
# from users the daemon doesn't trust, so the cache needs no key.
# Files, all at the top level: nix-cache-info, <hash>.narinfo and <hash>.nar.zst per store path
# (<hash> of the path), closure-<commit> (its store paths) and commit-<commit> (the output's store
# path, written last).
set -euo pipefail
(($# == 3)) || { sed -n '2,11p' "$0" | cut -c3-; exit 2; }
path=$1 commit=$2 target=$3
keep=${NIX_CACHE_KEEP:-30}
release='nix-cache'
nix=(nix --extra-experimental-features nix-command)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

if [[ $target == github:* ]]; then
    repo=${target#github:}
    if ! release_id=$(gh api "repos/$repo/releases/tags/$release" --jq .id 2>/dev/null); then
        gh release create "$release" --repo "$repo" --latest=false --title 'Nix binary cache' \
            --notes 'Binary cache of packaging/port.nix for the commits on master (tools/nix_cache.sh).' >/dev/null
        release_id=$(gh api "repos/$repo/releases/tags/$release" --jq .id)
    fi
    # NAME<TAB>ID, newest first.
    list() {
        gh api --paginate "repos/$repo/releases/$release_id/assets?per_page=100" \
            --jq '.[] | [.created_at, .name, .id] | @tsv' | sort -r | cut -f2-
    }
    upload() { gh release upload "$release" --repo "$repo" --clobber "$@" >/dev/null; }
    fetch() { gh release download "$release" --repo "$repo" --pattern "$1" --output -; }
    remove() { gh api -X DELETE "repos/$repo/releases/assets/$2" >/dev/null; }
else
    mkdir -p "$target"
    list() { (cd "$target" && ls -t | while read -r name; do printf '%s\t%s\n' "$name" "$name"; done); }
    upload() { cp "$@" "$target/"; }
    fetch() { cat "$target/$1"; }
    remove() { rm -f "$target/$2"; }
fi

ca=$("${nix[@]}" store make-content-addressed --json "$path" | python3 -c '
import json, sys
print(json.load(sys.stdin)["rewrites"][sys.argv[1]])' "$path")
"${nix[@]}" path-info --recursive "$ca" > "$work/closure-$commit"
list > "$work/assets"
present() { cut -f1 "$work/assets" | grep -qxF "$1"; }

missing=()
while read -r store_path; do
    hash=$(basename "$store_path" | cut -d- -f1)
    present "$hash.narinfo" || missing+=("$hash")
done < "$work/closure-$commit"
if ((${#missing[@]})); then
    # The whole closure: a binary cache only takes a path whose references it already holds.
    "${nix[@]}" copy --to "file://$work/cache?compression=zstd" "$ca"
    nars=() narinfos=()
    for hash in "${missing[@]}"; do
        info=$work/cache/$hash.narinfo
        nar=$(sed -n 's|^URL: ||p' "$info")
        mv "$work/cache/$nar" "$work/$hash.nar.zst"
        sed "s|^URL: .*|URL: $hash.nar.zst|" "$info" > "$work/$hash.narinfo"
        nars+=("$work/$hash.nar.zst") narinfos+=("$work/$hash.narinfo")
    done
    # A path's narinfo only once its NAR is there.
    upload "${nars[@]}"
    upload "${narinfos[@]}"
fi
if ! present nix-cache-info; then
    printf 'StoreDir: /nix/store\nWantMassQuery: 0\nPriority: 50\n' > "$work/nix-cache-info"
    upload "$work/nix-cache-info"
fi
echo "$ca" > "$work/commit-$commit"
upload "$work/closure-$commit"
upload "$work/commit-$commit"
echo "Published $ca for $commit (${#missing[@]} new store paths)"

list > "$work/assets"
grep '^commit-' "$work/assets" | cut -f1 | sed 's/^commit-//' > "$work/commits"
# This commit's own closure whatever the listing says: a fresh upload may not be listed yet.
cp "$work/closure-$commit" "$work/live"
while read -r kept; do
    # Without a kept commit's closure, its paths can't be told apart: nothing is pruned.
    fetch "closure-$kept" >> "$work/live" || { echo "No closure-$kept: not pruning" >&2; exit 0; }
done < <(head -n "$keep" "$work/commits")
xargs -rn1 basename < "$work/live" | cut -d- -f1 | sort -u > "$work/live-hashes"
tail -n +"$((keep + 1))" "$work/commits" > "$work/old"
pruned=0
while IFS=$'\t' read -r name id; do
    case $name in
        commit-* | closure-*) grep -qxF "${name#*-}" "$work/old" || continue ;;
        *.narinfo | *.nar.zst) grep -qxF "${name%%.*}" "$work/live-hashes" && continue ;;
        *) continue ;;
    esac
    remove "$name" "$id"
    pruned=$((pruned + 1))
done < "$work/assets"
echo "Pruned $pruned files; $(($(wc -l < "$work/assets") - pruned)) left (a release holds at most 1000)"
