#!/system/bin/sh

APP_NAME=neopanel_android
RUN_DIR=/data/local/tmp/neopanel

chmod 755 "$RUN_DIR/$APP_NAME" 2>/dev/null
pkill -x "$APP_NAME" 2>/dev/null

cd "$RUN_DIR" || exit 1

"./$APP_NAME" > "$RUN_DIR/$APP_NAME.log" 2>&1 &
