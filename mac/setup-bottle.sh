#!/bin/bash
# Creates a CrossOver bottle that runs one OpenParrot game. See mac/README.md.
set -euo pipefail

WINE_VERSION=11.21
WINE_ARCHIVE=wine-devel-$WINE_VERSION-osx64.tar.xz
WINE_URL=https://github.com/Gcenx/macOS_Wine_builds/releases/download/$WINE_VERSION/$WINE_ARCHIVE
WINE_SHA256=9909b486657e10ab66c7279371ae1f990345b596b3b1b3a0cf21f28af24fa0ce
DIRECTMUSIC_DLLS=(dmusic dmime dmband dmloader dmstyle dmcompos dswave dmscript dmusic32 dmsynth)

HERE=$(cd "$(dirname "$0")" && pwd)
CX=/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin
BOTTLE=OpenParrot
PROFILE=
GAME=
OPENPARROT=
SOUNDBANK=
ARCHIVE=

usage()
{
	cat <<EOF
Usage: $0 --game <zip or folder> --openparrot <OpenParrot-Win32.zip> --soundbank <.sf2 or .dls>
          [--profile <name in mac/profiles>] [--bottle <name>] [--wine-archive <$WINE_ARCHIVE>]
EOF
	exit 1
}

while [ $# -gt 0 ]; do
	[ $# -ge 2 ] || usage
	case "$1" in
		--game) GAME=$2 ;;
		--openparrot) OPENPARROT=$2 ;;
		--soundbank) SOUNDBANK=$2 ;;
		--profile) PROFILE=$2 ;;
		--bottle) BOTTLE=$2 ;;
		--wine-archive) ARCHIVE=$2 ;;
		*) usage ;;
	esac
	shift 2
done

[ -n "$GAME" ] && [ -n "$OPENPARROT" ] && [ -n "$SOUNDBANK" ] || usage
[ -x "$CX/wine" ] || { echo "CrossOver was not found in /Applications."; exit 1; }
case "$SOUNDBANK" in
	*.sf2|*.SF2|*.dls|*.DLS) ;;
	*) echo "The sound bank must be a .sf2 or .dls file."; exit 1 ;;
esac

BOTTLE_DIR="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/openparrot-mac.XXXXXX")
trap 'rm -r -- "$WORK"' EXIT

if [ ! -d "$BOTTLE_DIR" ]; then
	echo "Creating bottle $BOTTLE"
	"$CX/cxbottle" --bottle "$BOTTLE" --create --template win7_64 --description "OpenParrot"
fi
GAME_DIR="$BOTTLE_DIR/drive_c/game"
SYSWOW64="$BOTTLE_DIR/drive_c/windows/syswow64"
SYSTEM32="$BOTTLE_DIR/drive_c/windows/system32"
export WINEPREFIX="$BOTTLE_DIR"

wine_reg()
{
	"$CX/wine" --bottle "$BOTTLE" --cx-app reg.exe "$@" > /dev/null 2>&1
}

echo "Copying the game"
mkdir -p "$GAME_DIR"
if [ -d "$GAME" ]; then
	cp -R "$GAME/." "$GAME_DIR/"
else
	unzip -qo "$GAME" -d "$GAME_DIR"
fi
[ -f "$GAME_DIR/game.exe" ] || { echo "game.exe was not found at the top level of the game files."; exit 1; }

echo "Installing OpenParrot"
mkdir "$WORK/openparrot"
unzip -qo "$OPENPARROT" -d "$WORK/openparrot"
if [ -f "$GAME_DIR/iDmacDrv32.dll" ] && [ ! -f "$GAME_DIR/iDmacDrv32.dll.bundled" ]; then
	cp "$GAME_DIR/iDmacDrv32.dll" "$GAME_DIR/iDmacDrv32.dll.bundled"
fi
for file in OpenParrot.dll OpenParrotLoader.exe OpenParrotInput.exe iDmacDrv32.dll; do
	cp "$WORK/openparrot/$file" "$GAME_DIR/"
done
[ -f "$GAME_DIR/teknoparrot.ini" ] || printf '[General]\r\nWindowed=1\r\n' > "$GAME_DIR/teknoparrot.ini"
if [ -n "$PROFILE" ]; then
	cp "$HERE/profiles/$PROFILE.ini" "$GAME_DIR/OpenParrotInput.ini"
fi

if [ -z "$ARCHIVE" ]; then
	CACHE="$HOME/Library/Caches/openparrot-mac"
	ARCHIVE="$CACHE/$WINE_ARCHIVE"
	if [ ! -f "$ARCHIVE" ]; then
		echo "Downloading unsigned Wine $WINE_VERSION from Gcenx/macOS_Wine_builds"
		mkdir -p "$CACHE"
		curl -fL "$WINE_URL" -o "$ARCHIVE.part"
		mv "$ARCHIVE.part" "$ARCHIVE"
	fi
fi
ACTUAL=$(shasum -a 256 "$ARCHIVE" | cut -d' ' -f1)
[ "$ACTUAL" = "$WINE_SHA256" ] || { echo "Checksum mismatch for $ARCHIVE"; exit 1; }

echo "Installing the Wine $WINE_VERSION DirectMusic DLLs"
mkdir "$WORK/wine"
for dll in "${DIRECTMUSIC_DLLS[@]}"; do
	member="Wine Devel.app/Contents/Resources/wine/lib/wine/i386-windows/$dll.dll"
	tar -xf "$ARCHIVE" -C "$WORK/wine" "$member"
	[ -f "$SYSWOW64/$dll.dll.cxbackup" ] || cp "$SYSWOW64/$dll.dll" "$SYSWOW64/$dll.dll.cxbackup"
	python3 -I "$HERE/clear-builtin-marker.py" "$WORK/wine/$member" "$SYSWOW64/$dll.dll"
	wine_reg add 'HKCU\Software\Wine\DllOverrides' /v "$dll" /t REG_SZ /d native /f
done

echo "Installing the sound bank"
BANK="$WORK/gm.dls"
case "$SOUNDBANK" in
	*.sf2|*.SF2) python3 -I "$HERE/fix-soundfont.py" "$SOUNDBANK" "$BANK" ;;
	*) cp "$SOUNDBANK" "$BANK" ;;
esac
for dir in "$SYSTEM32" "$SYSWOW64"; do
	mkdir -p "$dir/drivers"
	cp "$BANK" "$dir/drivers/gm.dls"
done

"$CX/wineserver" -k || true
echo "Done. Start the game with: $HERE/play.sh --bottle $BOTTLE"
