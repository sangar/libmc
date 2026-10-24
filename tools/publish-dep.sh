#!/bin/sh
# Publishes deps/<name> as the release asset its line in deps.lock points at,
# so the build can fetch it. Run it from the repository root once deps/<name>
# holds the new source, trimmed to what the build needs plus the license, and
# its line in deps.lock has the new version, URL and ./nob hash deps/<name>.
# Pass the upstream URL the source came from for the release notes:
#
#     tools/publish-dep.sh curl https://github.com/curl/curl/releases/download/curl-8_23_0/curl-8.23.0.tar.xz
#
# It needs gh with write access to the repository. A published asset is never
# replaced: changed content gets a new version.
set -eu
name="$1"
upstream="$2"
read -r _ version url hash <<EOF
$(awk -v name="$name" '$1 == name && NF == 4' deps.lock)
EOF
tag="deps-$name-$version"
asset="$name-$version.tar.gz"
case "$url" in
  */releases/download/"$tag/$asset") ;;
  *) echo "deps.lock points $name at '$url', not at a release asset $tag/$asset" >&2; exit 1 ;;
esac
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# COPYFILE_DISABLE and --no-xattrs keep macOS tar from adding ._ files, which would change the hash, and
# extended attributes, which GNU tar warns about.
COPYFILE_DISABLE=1 tar --no-xattrs --exclude .DS_Store -czf "$work/$asset" -C deps "$name"
mkdir "$work/unpacked"
tar -xzf "$work/$asset" -C "$work/unpacked"
# The hash is the last line: ./nob prints its own rebuild first when tools/build.c changed.
actual="$(./nob hash "$work/unpacked/$name" | tail -n 1)"
if [ "$actual" != "$hash" ]; then
  echo "$asset holds $actual, deps.lock says $hash" >&2
  exit 1
fi
gh release create "$tag" "$work/$asset" --latest=false --title "$name $version" \
  --notes "$name $version from $upstream, trimmed to what libmc builds. Fetched by the build through deps.lock."
