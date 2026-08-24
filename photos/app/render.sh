#!/usr/bin/env bash
# Renders README screenshots of all VitaocyThings modules using Rack's -t mode.
# Output: <plugin root>/photos/<Module Name>.png
#
# Rack needs: -u <user dir> -t <zoom>. It screenshots every module found in the
# user dir's plugins folder, so we give it a temporary user dir containing only
# this plugin. Screenshots land in <user>/screenshots/<slug>/<module slug>.png.

set -e

# Make sure the MSYS2 toolchain (jq, tar, cp) is reachable
case ":$PATH:" in
	*:/c/msys64/mingw64/bin:*)
		;;
	*)
		export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
		;;
esac

RACK_EXE=${RACK_EXE:-"/d/FRUITY PATH/vcv/Rack2Free/Rack.exe"}
PLUGIN_SLUG="VitaocyThings"
ZOOM=${PHOTO_ZOOM:-2}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PHOTO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PLUGIN_DIR="$(cd "$PHOTO_DIR/.." && pwd)"

# Temporary user folder containing only our plugin
USER_DIR="$SCRIPT_DIR/.user"
PLUGIN_COPY="$USER_DIR/plugins-win-x64/VitaocyThings"

rm -rf "$USER_DIR"
mkdir -p "$PLUGIN_COPY"

# Copy the plugin without the heavy/unneeded folders
tar -C "$PLUGIN_DIR" --exclude=scl --exclude=build --exclude=dep --exclude=.git --exclude=photos -cf - . | tar -C "$PLUGIN_COPY" -xf -

# A few sample scales so SCL QNT has something to display
mkdir -p "$PLUGIN_COPY/scl"
find "$PLUGIN_DIR/scl" -maxdepth 1 -name "*.scl" | head -5 | xargs -r cp -t "$PLUGIN_COPY/scl/"

# Rack renders every module of the plugin to <user>/screenshots/<slug>/<module slug>.png
"$RACK_EXE" -u "$USER_DIR" -t "$ZOOM"

# Move the screenshots to photos/, named after the module names without spaces
mkdir -p "$PHOTO_DIR"
rm -f "$PHOTO_DIR"/*.png
jq -r '.modules[] | .slug + "|" + .name' "$PLUGIN_DIR/plugin.json" | while IFS='|' read -r slug name; do
	out="$(printf '%s' "$name" | tr -cd '[:alnum:]')"
	cp "$USER_DIR/screenshots/$PLUGIN_SLUG/$slug.png" "$PHOTO_DIR/$out.png"
done

# Clean up the temporary user folder
rm -rf "$USER_DIR"
