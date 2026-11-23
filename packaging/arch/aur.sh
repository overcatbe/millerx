#!/bin/bash
# The AUR package from packaging/arch/PKGBUILD, after a release is published:
#   packaging/arch/aur.sh 0.8.2 ~/Projects/millerx-aur
# It takes the release tarball (not GitHub's archive link) with its real sha256 and writes
# PKGBUILD + .SRCINFO into the AUR clone. Then: cd there, git commit, git push.
set -e
ver=$1; out=$2
[ -n "$ver" ] && [ -d "$out" ] || { echo "usage: $0 VERSION AUR-DIR" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
url="https://github.com/overcatbe/millerx/releases/download/v$ver/millerx-$ver.tar.gz"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
curl -sfL -A 'millerx-aur/1.0 (contact@example.org)' -o "$tmp/src.tar.gz" "$url"
sum=$(sha256sum "$tmp/src.tar.gz" | cut -d' ' -f1)
sed -e "s|^pkgver=.*|pkgver=$ver|" \
    -e "s|^source=.*|source=(\"\$pkgname-\$pkgver.tar.gz::\$url/releases/download/v\$pkgver/\$pkgname-\$pkgver.tar.gz\")|" \
    -e "s|^sha256sums=.*|sha256sums=('$sum')|" "$here/PKGBUILD" > "$out/PKGBUILD"
# .SRCINFO, as makepkg --printsrcinfo writes it (no makepkg needed)
(
  source "$out/PKGBUILD"
  printf 'pkgbase = %s\n' "$pkgname"
  printf '\tpkgdesc = %s\n\tpkgver = %s\n\tpkgrel = %s\n\turl = %s\n' "$pkgdesc" "$pkgver" "$pkgrel" "$url"
  for a in "${arch[@]}"; do printf '\tarch = %s\n' "$a"; done
  for a in "${license[@]}"; do printf '\tlicense = %s\n' "$a"; done
  for a in "${makedepends[@]}"; do printf '\tmakedepends = %s\n' "$a"; done
  for a in "${depends[@]}"; do printf '\tdepends = %s\n' "$a"; done
  for a in "${optdepends[@]}"; do printf '\toptdepends = %s\n' "$a"; done
  for a in "${options[@]}"; do printf '\toptions = %s\n' "$a"; done
  for a in "${source[@]}"; do printf '\tsource = %s\n' "$a"; done
  for a in "${sha256sums[@]}"; do printf '\tsha256sums = %s\n' "$a"; done
  printf '\npkgname = %s\n' "$pkgname"
) > "$out/.SRCINFO"
echo "AUR files for $ver written to $out (sha256 $sum)"
