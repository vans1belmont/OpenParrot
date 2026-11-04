#!/bin/bash
# Starts a game set up by setup-bottle.sh and cleans up the bottle when the game closes.
set -uo pipefail

CX=/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin
BOTTLE=OpenParrot
if [ $# -eq 2 ] && [ "$1" = "--bottle" ]; then
	BOTTLE=$2
elif [ $# -ne 0 ]; then
	echo "Usage: $0 [--bottle <name>]"
	exit 1
fi

BOTTLE_DIR="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE"
GAME_DIR="$BOTTLE_DIR/drive_c/game"

# Bottles from before this file existed run a 32-bit game.exe.
EXE=game.exe
LOADER=OpenParrotLoader.exe
CORE=OpenParrot
EXTRA_ENV=
if [ -f "$GAME_DIR/openparrot-mac.conf" ]; then
	. "$GAME_DIR/openparrot-mac.conf"
fi
[ -f "$GAME_DIR/$LOADER" ] || { echo "Bottle $BOTTLE is not set up. Run setup-bottle.sh first."; exit 1; }

# The default injection mode opens a crash dialog under Wine on macOS, and key
# and window events need the message pump.
export WINEPREFIX="$BOTTLE_DIR" WINEDEBUG=-all TP_REMOTETHREAD=1 TP_PUMP_MESSAGES=1
for setting in $EXTRA_ENV; do
	export "$setting"
done
if [ -d "$BOTTLE_DIR/gst-libav" ]; then
	export GST_PLUGIN_PATH="$BOTTLE_DIR/gst-libav/lib/gstreamer-1.0"
fi

GAME_PATTERN="^C:.game.${EXE//./\\.}"

game_running()
{
	pgrep -f "$GAME_PATTERN" > /dev/null
}

if game_running; then
	echo "A game is already running."
	exit 1
fi

cd "$GAME_DIR" || exit 1

# OpenParrot only tries to open the input pipe once, so the helper starts first.
"$CX/wine" --bottle "$BOTTLE" --cx-app 'C:\game\OpenParrotInput.exe' > /dev/null 2>&1 &
sleep 4
"$CX/wine" --bottle "$BOTTLE" --cx-app "C:\\game\\$LOADER" "$CORE" "C:\\game\\$EXE" > /dev/null 2>&1 &

for _ in $(seq 1 60); do
	game_running && break
	sleep 1
done

if game_running; then
	echo "Game started. Close its window to finish."
	while game_running; do
		sleep 1
	done
else
	echo "The game did not start."
fi

"$CX/wineserver" -k
