#!/usr/bin/env bash
# tools/win32-publish.sh ZIP TAG TARGET: attaches ZIP to the tag's release, where decanter downloads it
# without a token (kb #920, #926): for a vX.Y.Z tag, the Windows package tools/win32-bundle.sh made, as
# psport-windows-TAG-x86_64.zip; for a toolchain-win32-N tag, the toolchain package
# tools/win32-toolchain.sh made (psport-toolchain-win32-N-x86_64.zip) and its manifest beside it (.json);
# then the zip's .sha256 (sha256sum's format). The release is created when the Release workflow has not
# made it yet, with the tag's annotation as its notes and not marked latest, which that workflow sets
# when it gets there (never for a toolchain). A release that already has the .sha256 keeps its files,
# since users may have checked them. TARGET: github:OWNER/REPO (gh, with GH_TOKEN), or a local directory
# with a folder per release (TARGET/TAG/).
set -euo pipefail
(($# == 3)) || { sed -n '2,10p' "$0" | cut -c3-; exit 2; }
zip=$1 tag=$2 target=$3
[[ -f $zip ]] || { echo "No package: $zip" >&2; exit 2; }
extra=()
if [[ $tag =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    name=psport-windows-$tag-x86_64.zip
elif [[ $tag =~ ^toolchain-win32-[1-9][0-9]*$ ]]; then
    name=psport-$tag-x86_64.zip
    [[ ${zip##*/} == "$name" ]] || { echo "$zip is not $name, the package of $tag" >&2; exit 2; }
    [[ -f ${zip%.zip}.json ]] || { echo "No manifest beside the package: ${zip%.zip}.json" >&2; exit 2; }
    extra=("${zip%.zip}.json")
else
    echo "Not a vX.Y.Z or toolchain-win32-N tag: $tag" >&2; exit 2
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

if [[ $target == github:* ]]; then
    repo=${target#github:}
    # As the Release workflow writes them, so that whichever of the two comes first the notes are the same.
    create() {
        local object
        object=$(gh api "repos/$repo/git/ref/tags/$tag" --jq '.object | [.type, .sha] | @tsv') || return
        if [[ $object == tag$'\t'* ]]; then
            gh api "repos/$repo/git/tags/${object#*$'\t'}" --jq .message > "$work/message" || return
            sed '/^-----BEGIN \(PGP\|SSH\) SIGNATURE-----$/,$d' "$work/message" > "$work/notes"
        else
            : > "$work/notes"
        fi
        gh release create "$tag" --repo "$repo" --verify-tag --title "$tag" --notes-file "$work/notes" \
            --latest=false >/dev/null
    }
    # The other workflow may create it between the check and the creation.
    ensure_release() {
        gh release view "$tag" --repo "$repo" >/dev/null 2>&1 || create || gh release view "$tag" --repo "$repo" >/dev/null
    }
    assets() { gh release view "$tag" --repo "$repo" --json assets --jq '.assets[].name'; }
    upload() { gh release upload "$tag" --repo "$repo" "$@" >/dev/null; }
else
    dir=$target/$tag
    ensure_release() { mkdir -p -- "$dir"; }
    assets() { ls -A -- "$dir"; }
    upload() { cp -- "${!#}" "$dir/"; }
fi

ensure_release
# Read from a file: piped into grep -q, the listing can die of SIGPIPE, which pipefail reports as absent.
assets > "$work/assets"
if grep -qxF -- "$name.sha256" "$work/assets"; then
    echo "Kept $name and what went with it, already published in the release $tag"
    exit 0
fi
cp -- "$zip" "$work/$name"
(cd -- "$work" && sha256sum -- "$name" > "$name.sha256")
# The checksum last: decanter looks for it first, so once it is there the zip is too. Without it a zip
# left by a stopped run is not published yet, so it is replaced.
upload --clobber "$work/$name"
for file in "${extra[@]}"; do upload --clobber "$file"; done
upload "$work/$name.sha256"
echo "Published $name and $name.sha256${extra:+, with ${extra##*/},} in the release $tag"
