#!/usr/bin/env bash
# tools/win32-publish.sh ZIP TAG TARGET: attaches ZIP, the Windows package tools/win32-bundle.sh made
# for the tag TAG (vX.Y.Z), to the tag's release, where decanter downloads it without a token (kb #920):
# psport-windows-TAG-x86_64.zip, then its .sha256 (sha256sum's format). The release is created when the
# Release workflow has not made it yet, with the tag's annotation as its notes and not marked latest,
# which that workflow sets when it gets there. A release that already has the .sha256 keeps both files,
# since users may have checked them. TARGET: github:OWNER/REPO (gh, with GH_TOKEN), or a local directory
# with a folder per release (TARGET/TAG/).
set -euo pipefail
(($# == 3)) || { sed -n '2,8p' "$0" | cut -c3-; exit 2; }
zip=$1 tag=$2 target=$3
[[ -f $zip ]] || { echo "No package: $zip" >&2; exit 2; }
[[ $tag =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "Not a vX.Y.Z tag: $tag" >&2; exit 2; }
name=psport-windows-$tag-x86_64.zip
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
    echo "Kept $name and $name.sha256 already published in the release $tag"
    exit 0
fi
cp -- "$zip" "$work/$name"
(cd -- "$work" && sha256sum -- "$name" > "$name.sha256")
# The checksum last: decanter looks for it first, so once it is there the zip is too. Without it a zip
# left by a stopped run is not published yet, so it is replaced.
upload --clobber "$work/$name"
upload "$work/$name.sha256"
echo "Published $name and $name.sha256 in the release $tag"
