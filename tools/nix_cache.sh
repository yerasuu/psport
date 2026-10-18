#!/usr/bin/env bash
# tools/nix_cache.sh STORE_PATH COMMIT TAG TARGET: publishes STORE_PATH's closure (nix-build
# packaging/port.nix) as a flat Nix binary cache, for the release TAG (vX.Y.Z) at COMMIT, or with TAG
# "-" for COMMIT as a develop/main commit. TARGET: github:OWNER/REPO, the assets of that repository's
# release "nix-cache" (gh, with GH_TOKEN), or a local directory with the same files. First the
# releases beyond the newest NIX_CACHE_KEEP (default 30), the branch commits beyond the newest
# NIX_CACHE_KEEP_COMMITS (default 20), or whatever goes beyond 1000 files, and the commits and paths
# only they used are pruned; then only the paths the target lacks are uploaded.
# The closure is made content-addressed first: Nix accepts those paths without a signature, also
# from users the daemon doesn't trust, so the cache needs no key.
# Files, all at the top level: nix-cache-info, <hash>.narinfo and <hash>.nar.zst per store path
# (<hash> of the path), closure-<commit> (its store paths), commit-<commit> (the output's store
# path), release-<tag> (its commit), branch-<commit> (a branch commit, uploaded after its commit-)
# and latest-release ("<tag> <commit> <store path>" of the highest published version, written last).
set -euo pipefail
(($# == 4)) || { sed -n '2,15p' "$0" | cut -c3-; exit 2; }
path=$1 commit=$2 tag=$3 target=$4
[[ $commit =~ ^[0-9a-f]{40}$ ]] || { echo "Not a commit: $commit" >&2; exit 2; }
[[ $tag == - || $tag =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "Not a vX.Y.Z tag or -: $tag" >&2; exit 2; }
keep=${NIX_CACHE_KEEP:-30}
keep_commits=${NIX_CACHE_KEEP_COMMITS:-20}
release='nix-cache'
nix=(nix --extra-experimental-features nix-command)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

if [[ $target == github:* ]]; then
    repo=${target#github:}
    if ! release_id=$(gh api "repos/$repo/releases/tags/$release" --jq .id 2>/dev/null); then
        gh release create "$release" --repo "$repo" --latest=false --title 'Nix binary cache' \
            --notes 'Binary cache of packaging/port.nix for the vX.Y.Z releases and develop/main commits (tools/nix_cache.sh).' >/dev/null
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
# A branch commit already published (by its release, or pushed to the other branch) keeps the build
# there: replacing it could drop the paths a release's latest-release names.
published=0
if [[ $tag == - ]] && grep -qxF "commit-$commit" "$work/names" && fetch "closure-$commit" > "$work/published"; then
    published=1
    hashes < "$work/published" | sort -u > "$work/own-hashes"
    cp "$work/own-hashes" "$work/live-hashes"
fi

# Pruned before uploading, since a release holds at most 1000 files and a full one takes no more:
# this commit, the release latest-release names, the newest branch commits up to
# NIX_CACHE_KEEP_COMMITS and the newest other releases up to NIX_CACHE_KEEP, as many as fit with
# their paths, in that order. Commits neither a kept release nor a kept branch names go.
: > "$work/kept-tags"
: > "$work/kept-branches"
echo "$commit" > "$work/kept-commits"
if [[ $tag == - ]]; then echo "$commit" > "$work/kept-branches"; else echo "$tag" > "$work/kept-tags"; fi
latest_tag=
if grep -qxF latest-release "$work/names"; then latest_tag=$(fetch latest-release | cut -d' ' -f1); fi
awk -F'\t' '$1 ~ /^release-/ { print substr($1, 9) }' "$work/assets" |
    awk -v own="$tag" -v latest="$latest_tag" '$0 != own && $0 != latest && !seen[$0]++' > "$work/tags"
awk -F'\t' '$1 ~ /^branch-/ { print substr($1, 8) }' "$work/assets" |
    awk -v own="$(head -n1 "$work/kept-branches")" '$0 != own && !seen[$0]++' > "$work/branches"
# Files the kept set takes with the paths in $1, plus nix-cache-info and latest-release.
files() {
    echo $(($(wc -l < "$1") * 2 + $(wc -l < "$work/kept-commits") * 2 + $(wc -l < "$work/kept-tags") +
        $(wc -l < "$work/kept-branches") + 2))
}
# Keeps commit $1 and its paths when they fit with its commit- and closure- and $2 more files; fails
# with 2 when its closure is missing, since its paths can't then be told apart.
keep_commit() {
    grep -qxF "$1" "$work/kept-commits" && return 0
    fetch "closure-$1" > "$work/other" || { echo "No closure-$1: not pruning" >&2; return 2; }
    hashes < "$work/other" | sort -u - "$work/live-hashes" > "$work/with"
    (($(files "$work/with") + 2 + $2 <= 1000)) || return 1
    mv "$work/with" "$work/live-hashes"
    echo "$1" >> "$work/kept-commits"
}
prune=1
keep_release() {
    # Without a kept release's commit, its paths can't be told apart either.
    other_commit=$(fetch "release-$1") || { echo "No release-$1: not pruning" >&2; return 2; }
    keep_commit "$other_commit" 1 || return
    echo "$1" >> "$work/kept-tags"
}
status=0
if [[ -n $latest_tag && $latest_tag != "$tag" ]]; then keep_release "$latest_tag" || status=$?; fi
while ((status != 2)) && read -r other && (($(wc -l < "$work/kept-branches") < keep_commits)); do
    keep_commit "$other" 1 || { status=$?; break; }
    echo "$other" >> "$work/kept-branches"
done < "$work/branches"
while ((status != 2)) && read -r other && (($(wc -l < "$work/kept-tags") < keep)); do
    keep_release "$other" || { status=$?; break; }
done < "$work/tags"
((status == 2)) && prune=0
pruned=0
while ((prune)) && IFS=$'\t' read -r name id; do
    case $name in
        commit-* | closure-*) grep -qxF "${name#*-}" "$work/kept-commits" && continue ;;
        release-*) grep -qxF "${name#release-}" "$work/kept-tags" && continue ;;
        branch-*) grep -qxF "${name#branch-}" "$work/kept-branches" && continue ;;
        *.narinfo | *.nar.zst) grep -qxF "${name%%.*}" "$work/live-hashes" && continue ;;
        *) continue ;;
    esac
    remove "$name" "$id"
    pruned=$((pruned + 1))
done < "$work/assets"

missing=()
if ((published)); then
    echo "$commit" > "$work/branch-$commit"
    upload "$work/branch-$commit"
    echo "Kept the build already published for $commit ($pruned files pruned)"
    exit 0
fi
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
upload "$work/closure-$commit"
upload "$work/commit-$commit"
if [[ $tag == - ]]; then
    echo "$commit" > "$work/branch-$commit"
    upload "$work/branch-$commit"
    echo "Published $ca for $commit (${#missing[@]} new store paths, $pruned files pruned)"
    exit 0
fi
echo "$commit" > "$work/release-$tag"
upload "$work/release-$tag"
# A tag pushed for an older version (a fix on an earlier line) is published without becoming latest.
if [[ $(printf '%s\n' "$latest_tag" | sort -V - "$work/tags" "$work/kept-tags" | tail -n1) == "$tag" ]]; then
    echo "$tag $commit $ca" > "$work/latest-release"
    upload "$work/latest-release"
    latest=", now latest-release"
fi
echo "Published $ca for $tag at $commit (${#missing[@]} new store paths, $pruned files pruned${latest:-})"
