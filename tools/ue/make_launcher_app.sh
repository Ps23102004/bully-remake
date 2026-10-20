#!/bin/bash
# OWNER: builder-B (ue)
# Builds a double-clickable launcher, "/Applications/Bully Remake.app" (or $1), that plays the Bullworth map in a
# normal game window: the UE 5.8 editor binary in -game mode on this checkout's uproject (uncooked; the same setup the
# render harness verifies). Needs UE_ROOT and this checkout to stay where they are. Log: ~/Library/Logs/BullyRemake.log
#   tools/ue/make_launcher_app.sh ["/Applications/Bully Remake.app"]
set -euo pipefail
source "$(dirname "$0")/_common.sh"

APP="${1:-/Applications/Bully Remake.app}"
UE_GAME="$UE_ROOT/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
[ -x "$UE_GAME" ] || br_die "UnrealEditor not found at $UE_GAME"

case "$APP" in *.app) ;; *) br_die "target must end in .app: $APP" ;; esac
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

cat > "$APP/Contents/MacOS/BullyRemake" <<EOF
#!/bin/bash
exec "$UE_GAME" "$BR_UPROJECT" /Game/Imported/Maps/Bullworth -game -windowed -ResX=1920 -ResY=1080 \\
  -nosplash -abslog="\$HOME/Library/Logs/BullyRemake.log" "\$@"
EOF
chmod +x "$APP/Contents/MacOS/BullyRemake"

cat > "$APP/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>Bully Remake</string>
  <key>CFBundleDisplayName</key><string>Bully Remake</string>
  <key>CFBundleIdentifier</key><string>local.bully-remake.launcher</string>
  <key>CFBundleExecutable</key><string>BullyRemake</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
EOF

# Icon from the owner's installed Anniversary Edition, if present (stays on this machine; never in git).
SRC_ICON="/Applications/bully.app/Wrapper/bully.app/AppIcon1024x1024.png"
if [ -f "$SRC_ICON" ]; then
  SET="$(mktemp -d)/AppIcon.iconset"; mkdir -p "$SET"
  for s in 16 32 128 256 512; do
    sips -z $s $s "$SRC_ICON" --out "$SET/icon_${s}x${s}.png" >/dev/null
    sips -z $((s*2)) $((s*2)) "$SRC_ICON" --out "$SET/icon_${s}x${s}@2x.png" >/dev/null
  done
  iconutil -c icns "$SET" -o "$APP/Contents/Resources/AppIcon.icns"
fi
touch "$APP"
echo "built $APP"
