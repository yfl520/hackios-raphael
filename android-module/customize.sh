#!/system/bin/sh
# Installation checks the target but never requests a reboot.
[ "$BOOTMODE" = true ] || abort '请在 Android 的 root 管理器内安装，不要通过 TWRP 刷入。'
MODDIR=$MODPATH
[ -r "$MODPATH/target.conf" ] || abort '缺少配套启动配置，请重新下载模块。'
[ -r "$MODPATH/switch-lib.sh" ] || abort '缺少检查脚本，请重新下载模块。'
. "$MODPATH/target.conf"
. "$MODPATH/switch-lib.sh"
ui_print '红米双系统 · 进入 iOS'
ui_print '酷安：你说喜欢是装的'
check_target || abort '安装检查未通过，请查看上方具体原因；本模块未刷写或重启手机。'
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/action.sh" 0 0 0755
ui_print '模块已安装。请按管理器提示重启 Android 一次。'
ui_print '需要进入 iOS 时，在模块卡片点击“操作 / Action”。'
