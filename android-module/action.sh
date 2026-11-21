#!/system/bin/sh
MODDIR=${0%/*}
. "$MODDIR/target.conf" || exit 1
. "$MODDIR/switch-lib.sh" || exit 1
switch_main "$@"
