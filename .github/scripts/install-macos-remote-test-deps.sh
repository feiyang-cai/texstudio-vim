#!/usr/bin/env bash
# Use the same Qt/Poppler binaries as CD. Homebrew's Poppler disables Qt6.
set -euo pipefail
qt_version=$(python3 -c 'import re; print(re.search(r"OSX_QT_VERSION: (\S+)", open(".github/workflows/cd.yml").read())[1])')
poppler_version=$(python3 -c 'import re; print(re.search(r"OSX_POPPLER_VERSION: (\S+)", open(".github/workflows/cd.yml").read())[1])')
suffix=Darwin
if [[ $(uname -m) == arm64 ]]; then suffix=Darwin-M1; fi
base="https://github.com/sunderme/homebrew-qt6-modules/releases/download/$qt_version"
qt_prefix="/usr/local/QtBase-$qt_version-Darwin"
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT
for module in QtBase QtSvg Qt5Compat QtTools QtDeclarative; do
    archive="$temporary/$module.tar.xz"
    curl --fail --location --retry 3 "$base/$module-$qt_version-$suffix.tar.xz" -o "$archive"
    if [[ $module == QtBase ]]; then
        sudo tar xf "$archive" -C /usr/local
    else
        sudo tar xf "$archive" -C "$qt_prefix" --strip-components=1
    fi
done
curl --fail --location --retry 3 "$base/poppler-$poppler_version-$suffix.tar.xz" -o "$temporary/poppler.tar.xz"
sudo tar xf "$temporary/poppler.tar.xz" -C /usr/local --strip-components=1
brew install cmake ninja hunspell pkg-config cairo fontconfig freetype gettext jpeg libpng libtiff little-cms2 openjpeg
echo "REMOTE_TEST_QT=$qt_prefix" >> "$GITHUB_ENV"
echo "PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$(brew --prefix)/lib/pkgconfig" >> "$GITHUB_ENV"
