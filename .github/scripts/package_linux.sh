#!/usr/bin/env sh

# Exit on errors
set -e

. ../.github/scripts/get-version.sh

echo "Running linuxdeployqt"

#wget -c -nv "https://github.com/probonopd/linuxdeployqt/releases/download/continuous/linuxdeployqt-continuous-x86_64.AppImage"
#chmod a+x linuxdeployqt-continuous-x86_64.AppImage
wget -c https://github.com/$(wget -q https://github.com/probonopd/go-appimage/releases/expanded_assets/continuous -O - | grep "appimagetool-.*-x86_64.AppImage" | head -n 1 | cut -d '"' -f 2)
chmod +x appimagetool-*.AppImage
unset QTDIR; unset QT_PLUGIN_PATH ; unset LD_LIBRARY_PATH
export VERSION=linux-${VERSION_NAME}
cp ../utilities/texstudio.svg appdir
./appimagetool-*.AppImage -s deploy appdir/usr/share/applications/*.desktop
sed -i -e'/export PYTH/d' appdir/AppRun # workaroun python issue #4061
# ./appimagetool-*.AppImage -s deploy appdir/usr/share/applications/*.desktop # Bundle EVERYTHING
# ./linuxdeployqt-continuous-x86_64.AppImage appdir/usr/share/applications/*.desktop -bundle-non-qt-libs -extra-plugins=iconengines/libqsvgicon.so -appimage
# ./linuxdeployqt-continuous-x86_64.AppImage appdir/usr/share/applications/*.desktop -appimage
# Standalone deployment can copy the ELF interpreter without its executable bit.
# AppRun executes it directly, so preserve execution permission in the image.
python3 - <<'PYLOADER'
from pathlib import Path
root = Path("appdir").resolve()
loaders = list(root.rglob("ld-linux*.so*"))
if not loaders:
    raise SystemExit("Standalone AppDir has no ELF interpreter")
for loader in loaders:
    target = loader.resolve(strict=True)
    if root not in target.parents:
        raise SystemExit(f"ELF interpreter resolves outside AppDir: {loader}")
    target.chmod(target.stat().st_mode | 0o111)
    print(f"Executable ELF interpreter: {target.relative_to(root)}")
PYLOADER
./appimagetool-*.AppImage ./appdir # create actual appimage
cp TeXstudio-${VERSION}-x86_64.AppImage ../texstudio-vim-${VERSION}-x86_64.AppImage
cp TeXstudio-${VERSION}-x86_64.AppImage ../texstudio-vim-${GIT_VERSION}-x86_64.AppImage
sha256sum appdir/usr/bin/texstudio 
sha256sum ../texstudio-vim-${VERSION}-x86_64.AppImage 


