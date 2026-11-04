#!/bin/bash
# Creates a CrossOver bottle that runs one OpenParrot game. See mac/README.md.
set -euo pipefail

WINE_VERSION=11.21
WINE_ARCHIVE=wine-devel-$WINE_VERSION-osx64.tar.xz
WINE_URL=https://github.com/Gcenx/macOS_Wine_builds/releases/download/$WINE_VERSION/$WINE_ARCHIVE
WINE_SHA256=9909b486657e10ab66c7279371ae1f990345b596b3b1b3a0cf21f28af24fa0ce
DIRECTMUSIC_DLLS=(dmusic dmime dmband dmloader dmstyle dmcompos dswave dmscript dmusic32 dmsynth)

GSTREAMER_PACKAGE=gstreamer-1.0-1.24.13-universal.pkg
GSTREAMER_SHA256=a18458c7cd61e5f2c71bb17c6eb88d54493adb964ec30dd53d96244aac537839
# What the libav plugin needs from the package and, through links, from CrossOver.
GSTREAMER_LIBRARIES=(libavfilter.9 libavformat.60 libavcodec.60 libavutil.58 libswscale.7 libswresample.4 libz.1 libbz2.1)
CROSSOVER_LIBRARIES=(libgstreamer-1.0.0 libgobject-2.0.0 libglib-2.0.0 libgstbase-1.0.0 libgstvideo-1.0.0 libgstaudio-1.0.0 libgstpbutils-1.0.0 libffi.8 libgmodule-2.0.0 libgsttag-1.0.0 libintl.8 libpcre2-8.0)

HERE=$(cd "$(dirname "$0")" && pwd)
CX=/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin
CX_LIBRARIES=/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/lib64
BOTTLE=OpenParrot
PROFILE=
GAME=
OPENPARROT=
INPUT_HELPER=
SOUNDBANK=
ARCHIVE=
EXE=game.exe
GSTREAMER=
SKIP_APM_STARTUP=
SHARED_COPY=

usage()
{
	cat <<EOF
Usage: $0 --game <zip or folder> --openparrot <OpenParrot zip> [options]
  --bottle <name>                bottle to create or update
  --profile <name>               key layout from mac/profiles
  --exe <name>                   game program, game.exe by default
  --soundbank <.sf2 or .dls>     install a MIDI sound bank and the Wine DirectMusic DLLs
  --wine-archive <file>          $WINE_ARCHIVE, if already downloaded
  --input-helper <OpenParrot-Win32.zip>  where to find OpenParrotInput.exe for a 64-bit game
  --gstreamer <file>             $GSTREAMER_PACKAGE, for WMV movies
  --skip-apm-startup             patch the bottle's copy of apm.dll
  --shared-copy                  turn on the D3D9 to D3D11 frame copy
EOF
	exit 1
}

while [ $# -gt 0 ]; do
	case "$1" in
		--skip-apm-startup) SKIP_APM_STARTUP=1; shift; continue ;;
		--shared-copy) SHARED_COPY=1; shift; continue ;;
	esac
	[ $# -ge 2 ] || usage
	case "$1" in
		--game) GAME=$2 ;;
		--openparrot) OPENPARROT=$2 ;;
		--input-helper) INPUT_HELPER=$2 ;;
		--soundbank) SOUNDBANK=$2 ;;
		--profile) PROFILE=$2 ;;
		--bottle) BOTTLE=$2 ;;
		--wine-archive) ARCHIVE=$2 ;;
		--exe) EXE=$2 ;;
		--gstreamer) GSTREAMER=$2 ;;
		*) usage ;;
	esac
	shift 2
done

[ -n "$GAME" ] && [ -n "$OPENPARROT" ] || usage
[ -x "$CX/wine" ] || { echo "CrossOver was not found in /Applications."; exit 1; }
case "$SOUNDBANK" in
	""|*.sf2|*.SF2|*.dls|*.DLS) ;;
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

install_directmusic()
{
	if [ -z "$ARCHIVE" ]; then
		local cache="$HOME/Library/Caches/openparrot-mac"
		ARCHIVE="$cache/$WINE_ARCHIVE"
		if [ ! -f "$ARCHIVE" ]; then
			echo "Downloading unsigned Wine $WINE_VERSION from Gcenx/macOS_Wine_builds"
			mkdir -p "$cache"
			curl -fL "$WINE_URL" -o "$ARCHIVE.part"
			mv "$ARCHIVE.part" "$ARCHIVE"
		fi
	fi
	local actual
	actual=$(shasum -a 256 "$ARCHIVE" | cut -d' ' -f1)
	[ "$actual" = "$WINE_SHA256" ] || { echo "Checksum mismatch for $ARCHIVE"; exit 1; }

	echo "Installing the Wine $WINE_VERSION DirectMusic DLLs"
	mkdir "$WORK/wine"
	for dll in "${DIRECTMUSIC_DLLS[@]}"; do
		local member="Wine Devel.app/Contents/Resources/wine/lib/wine/i386-windows/$dll.dll"
		tar -xf "$ARCHIVE" -C "$WORK/wine" "$member"
		[ -f "$SYSWOW64/$dll.dll.cxbackup" ] || cp "$SYSWOW64/$dll.dll" "$SYSWOW64/$dll.dll.cxbackup"
		python3 -I "$HERE/clear-builtin-marker.py" "$WORK/wine/$member" "$SYSWOW64/$dll.dll"
		wine_reg add 'HKCU\Software\Wine\DllOverrides' /v "$dll" /t REG_SZ /d native /f
	done

	echo "Installing the sound bank"
	local bank="$WORK/gm.dls"
	case "$SOUNDBANK" in
		*.sf2|*.SF2) python3 -I "$HERE/fix-soundfont.py" "$SOUNDBANK" "$bank" ;;
		*) cp "$SOUNDBANK" "$bank" ;;
	esac
	for dir in "$SYSTEM32" "$SYSWOW64"; do
		mkdir -p "$dir/drivers"
		cp "$bank" "$dir/drivers/gm.dls"
	done
}

install_gstreamer()
{
	local actual
	actual=$(shasum -a 256 "$GSTREAMER" | cut -d' ' -f1)
	[ "$actual" = "$GSTREAMER_SHA256" ] || { echo "Checksum mismatch for $GSTREAMER"; exit 1; }

	echo "Installing the GStreamer libav plugin"
	pkgutil --expand "$GSTREAMER" "$WORK/gstreamer-package"
	mkdir "$WORK/gstreamer"
	for payload in "$WORK"/gstreamer-package/*.pkg/Payload; do
		(cd "$WORK/gstreamer" && gzip -dc "$payload" | cpio -id --quiet '*lib/*.dylib') 2> /dev/null || true
	done

	local target="$BOTTLE_DIR/gst-libav/lib"
	mkdir -p "$target/gstreamer-1.0"
	cp "$WORK/gstreamer/lib/gstreamer-1.0/libgstlibav.dylib" "$target/gstreamer-1.0/"
	for library in "${GSTREAMER_LIBRARIES[@]}"; do
		cp "$WORK/gstreamer/lib/$library.dylib" "$target/"
	done
	for library in "${CROSSOVER_LIBRARIES[@]}"; do
		ln -sf "$CX_LIBRARIES/$library.dylib" "$target/$library.dylib"
	done
}

echo "Copying the game"
mkdir -p "$GAME_DIR"
if [ -d "$GAME" ]; then
	cp -R "$GAME/." "$GAME_DIR/"
else
	unzip -qo "$GAME" -d "$GAME_DIR"
fi
[ -f "$GAME_DIR/$EXE" ] || { echo "$EXE was not found at the top level of the game files."; exit 1; }

echo "Installing OpenParrot"
mkdir "$WORK/openparrot"
unzip -qo "$OPENPARROT" -d "$WORK/openparrot"
if [ -f "$WORK/openparrot/OpenParrot64.dll" ]; then
	LOADER=OpenParrotLoader64.exe
	CORE=OpenParrot64
	# A suspended 64-bit game can't be inspected under Wine, so the loader parks it instead.
	EXTRA_ENV="TP_ENTRYPOINT_REMOTETHREAD_MS=2000 TP_LOADER_MANAGED_INIT=1"
	for file in OpenParrot64.dll OpenParrotLoader64.exe iDmacDrv64.dll bngrw.dll; do
		cp "$WORK/openparrot/$file" "$GAME_DIR/"
	done
	[ -n "$INPUT_HELPER" ] || { echo "A 64-bit game needs --input-helper <OpenParrot-Win32.zip>."; exit 1; }
	unzip -qoj "$INPUT_HELPER" OpenParrotInput.exe -d "$GAME_DIR"
else
	LOADER=OpenParrotLoader.exe
	CORE=OpenParrot
	EXTRA_ENV=
	if [ -f "$GAME_DIR/iDmacDrv32.dll" ] && [ ! -f "$GAME_DIR/iDmacDrv32.dll.bundled" ]; then
		cp "$GAME_DIR/iDmacDrv32.dll" "$GAME_DIR/iDmacDrv32.dll.bundled"
	fi
	for file in OpenParrot.dll OpenParrotLoader.exe OpenParrotInput.exe iDmacDrv32.dll; do
		cp "$WORK/openparrot/$file" "$GAME_DIR/"
	done
fi
[ -f "$GAME_DIR/teknoparrot.ini" ] || printf '[General]\r\nWindowed=1\r\n' > "$GAME_DIR/teknoparrot.ini"
if [ -n "$PROFILE" ]; then
	cp "$HERE/profiles/$PROFILE.ini" "$GAME_DIR/OpenParrotInput.ini"
fi

if [ -n "$SKIP_APM_STARTUP" ]; then
	echo "Patching the bottle's copy of apm.dll"
	[ -f "$GAME_DIR/apm.dll.original" ] || cp "$GAME_DIR/apm.dll" "$GAME_DIR/apm.dll.original"
	python3 -I "$HERE/skip-dll-startup.py" "$GAME_DIR/apm.dll.original" "$GAME_DIR/apm.dll"
fi

if [ -n "$SHARED_COPY" ]; then
	EXTRA_ENV="$EXTRA_ENV TP_D3D9_SHARED_COPY=1"
fi

if [ -n "$SOUNDBANK" ]; then
	install_directmusic
fi

if [ -n "$GSTREAMER" ]; then
	install_gstreamer
fi

cat > "$GAME_DIR/openparrot-mac.conf" <<EOF
EXE="$EXE"
LOADER="$LOADER"
CORE="$CORE"
EXTRA_ENV="$EXTRA_ENV"
EOF

"$CX/wineserver" -k || true
echo "Done. Start the game with: $HERE/play.sh --bottle $BOTTLE"
