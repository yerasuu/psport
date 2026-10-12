#!/usr/bin/env bash
# tools/nix_cache.sh STORE_PATH COMMIT TAG TARGET: publishes STORE_PATH's closure (nix-build
# packaging/port.nix) as a flat Nix binary cache, for the release TAG (vX.Y.Z) at COMMIT. TARGET:
# github:OWNER/REPO, the assets of that repository's release "nix-cache" (gh, with GH_TOKEN), or a
# local directory with the same files. First the releases beyond the newest NIX_CACHE_KEEP (default
# 30), or beyond 1000 files, and the commits and paths only they used are pruned; then only the paths
# the target lacks are uploaded.
# The closure is made content-addressed first: Nix accepts those paths without a signature, also
# from users the daemon doesn't trust, so the cache needs no key.
# Files, all at the top level: nix-cache-info, <hash>.narinfo and <hash>.nar.zst per store path
# (<hash> of the path), closure-<commit> (its store paths), commit-<commit> (the output's store
# path), release-<tag> (its commit) and latest-release ("<tag> <commit> <store path>" of the highest
# published version, written last).
set -euo pipefail
(($# == 4)) || { sed -n '2,13p' "$0" | cut -c3-; exit 2; }
path=$1 commit=$2 tag=$3 target=$4
[[ $commit =~ ^[0-9a-f]{40}$ ]] || { echo "Not a commit: $commit" >&2; exit 2; }
[[ $tag =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "Not a vX.Y.Z tag: $tag" >&2; exit 2; }
keep=${NIX_CACHE_KEEP:-30}
release='nix-cache'
nix=(nix --extra-experimental-features nix-command)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

if [[ $target == github:* ]]; then
    repo=${target#github:}
    if ! release_id=$(gh api "repos/$repo/releases/tags/$release" --jq .id 2>/dev/null); then
        gh release create "$release" --repo "$repo" --latest=false --title 'Nix binary cache' \
            --notes 'Binary cache of packaging/port.nix for the vX.Y.Z releases (tools/nix_cache.sh).' >/dev/null
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
hashes() { xargs -rn1 basename | cut -d- -f1; }
hashes < "$work/closure-$commit" | sort -u > "$work/own-hashes"
cp "$work/own-hashes" "$work/live-hashes"
list > "$work/assets"
# Read from a file: piped into grep -q, cut can die of SIGPIPE, which pipefail reports as absent.
cut -f1 "$work/assets" > "$work/names"
# A path counts only with both files: a run stopped while pruning may have left one.
present() { grep -qxF "$1.narinfo" "$work/names" && grep -qxF "$1.nar.zst" "$work/names"; }

# Pruned before uploading, since a release holds at most 1000 files and a full one takes no more:
# this release, the one latest-release names and the newest others up to NIX_CACHE_KEEP, as many as
# fit with their paths. Commits no kept release names (master builds from before releases) go.
echo "$tag" > "$work/kept-tags"
echo "$commit" > "$work/kept-commits"
{
    if grep -qxF latest-release "$work/names"; then fetch latest-release | cut -d' ' -f1; fi
    awk -F'\t' '$1 ~ /^release-/ { print substr($1, 9) }' "$work/assets"
} | awk -v own="$tag" '$0 != own && !seen[$0]++' > "$work/tags"
prune=1
while read -r other && (($(wc -l < "$work/kept-tags") < keep)); do
    # Without a kept release's commit or closure, its paths can't be told apart: nothing is pruned.
    other_commit=$(fetch "release-$other") || { echo "No release-$other: not pruning" >&2; prune=0; break; }
    if ! grep -qxF "$other_commit" "$work/kept-commits"; then
        fetch "closure-$other_commit" > "$work/other" ||
            { echo "No closure-$other_commit: not pruning" >&2; prune=0; break; }
        hashes < "$work/other" | sort -u - "$work/live-hashes" > "$work/with"
        files=$(($(wc -l < "$work/with") * 2 + ($(wc -l < "$work/kept-commits") + 1) * 2))
        ((files + $(wc -l < "$work/kept-tags") + 1 + 2 <= 1000)) || break
        mv "$work/with" "$work/live-hashes"
        echo "$other_commit" >> "$work/kept-commits"
    fi
    echo "$other" >> "$work/kept-tags"
done < "$work/tags"
pruned=0
while ((prune)) && IFS=$'\t' read -r name id; do
    case $name in
        commit-* | closure-*) grep -qxF "${name#*-}" "$work/kept-commits" && continue ;;
        release-*) grep -qxF "${name#release-}" "$work/kept-tags" && continue ;;
        *.narinfo | *.nar.zst) grep -qxF "${name%%.*}" "$work/live-hashes" && continue ;;
        *) continue ;;
    esac
    remove "$name" "$id"
    pruned=$((pruned + 1))
done < "$work/assets"

missing=()
while read -r hash; do
    present "$hash" || missing+=("$hash")
done < "$work/own-hashes"
if ((${#missing[@]})); then
    # The whole closure: a binary cache only takes a path whose references it already holds.
    "${nix[@]}" copy --to "file://$work/cache?compression=zstd" "$ca"
    nars=() narinfos=()
    for hash in "${missing[@]}"; do
        info=$work/cache/$hash.narinfo
        nar=$(sed -n 's|^URL: ||p' "$info")
        # Copied: paths with the same contents share one NAR.
        cp "$work/cache/$nar" "$work/$hash.nar.zst"
        sed "s|^URL: .*|URL: $hash.nar.zst|" "$info" > "$work/$hash.narinfo"
        nars+=("$work/$hash.nar.zst") narinfos+=("$work/$hash.narinfo")
    done
    # A path's narinfo only once its NAR is there.
    upload "${nars[@]}"
    upload "${narinfos[@]}"
fi
if ! grep -qxF nix-cache-info "$work/names"; then
    printf 'StoreDir: /nix/store\nWantMassQuery: 0\nPriority: 50\n' > "$work/nix-cache-info"
    upload "$work/nix-cache-info"
fi
echo "$ca" > "$work/commit-$commit"
echo "$commit" > "$work/release-$tag"
upload "$work/closure-$commit"
upload "$work/commit-$commit"
upload "$work/release-$tag"
# A tag pushed for an older version (a fix on an earlier line) is published without becoming latest.
if [[ $(sort -V "$work/tags" "$work/kept-tags" | tail -n1) == "$tag" ]]; then
    echo "$tag $commit $ca" > "$work/latest-release"
    upload "$work/latest-release"
    latest=", now latest-release"
fi
echo "Published $ca for $tag at $commit (${#missing[@]} new store paths, $pruned files pruned${latest:-})"
