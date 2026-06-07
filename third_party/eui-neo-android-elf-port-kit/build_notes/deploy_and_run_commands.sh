#!/system/bin/sh

# Generic Android ELF deployment example.
# Adjust executable name and source paths according to your own build output.

APP_NAME=gallery
RUN_DIR=/data/local/tmp/eui_neo

mkdir -p "$RUN_DIR"

# Example:
# cp /path/to/build/output/$APP_NAME "$RUN_DIR/$APP_NAME"
# cp /path/to/libc++_shared.so "$RUN_DIR/libc++_shared.so"
# cp -a /path/to/assets "$RUN_DIR/assets"

chmod 755 "$RUN_DIR/$APP_NAME" 2>/dev/null

pkill -x "$APP_NAME" 2>/dev/null

cd "$RUN_DIR" || exit 1
export LD_LIBRARY_PATH="$RUN_DIR:$LD_LIBRARY_PATH"

"./$APP_NAME" > "$RUN_DIR/$APP_NAME.log" 2>&1 &
