#!/system/bin/sh
# Read-only validation; the only device action is Android's standard recovery reboot.

switch_message() {
    if command -v ui_print >/dev/null 2>&1; then ui_print "$1"; else printf '%s\n' "$1"; fi
}

switch_error() {
    switch_message "未切换：$1"
    return 1
}

is_block_device() { [ -b "$1" ]; }
reboot_available() { [ -x /system/bin/reboot ]; }

resolve_node() {
    for candidate in "/dev/block/by-name/$1" "/dev/block/bootdevice/by-name/$1"; do
        if is_block_device "$candidate"; then printf '%s\n' "$candidate"; return 0; fi
    done
    for entry in /sys/class/block/*/uevent; do
        [ -r "$entry" ] || continue
        grep -qx "PARTNAME=$1" "$entry" || continue
        device=${entry%/uevent}; device=${device##*/}
        if is_block_device "/dev/block/$device"; then printf '/dev/block/%s\n' "$device"; return 0; fi
    done
    return 1
}

is_unsigned() { case "$1" in ''|*[!0-9]*) return 1;; *) return 0;; esac; }

read_block() { dd if="$1" bs=4096 skip="$2" count=1 2>/dev/null; }

read_magic() {
    read_block "$1" "$2" | dd bs=1 skip="$3" count="$4" 2>/dev/null
}

check_resources() {
    [ "$(read_magic "$DARWIN_NODE" "$RESOURCE_BLOCK" 0 8)" = RAPHIOS1 ] || {
        switch_error '缺少 iOS 启动资源头，请用配套一键包更新后再试。'; return 1;
    }
    # Target ARM64 is little-endian. CRC fields vary with each phone's own NV.
    set -- $(read_block "$DARWIN_NODE" "$RESOURCE_BLOCK" | od -An -v -tu4 -j8 -N64 2>/dev/null)
    [ "$#" = 16 ] || { switch_error '无法完整读取启动资源头。'; return 1; }
    geometry="$1 $2 $3 $4 $5 $6 $7 $8 $9 ${10} ${11} ${12}"
    [ "$geometry" = "$RESOURCE_GEOMETRY" ] && [ "${16}" = 1 ] || {
        switch_error 'iOS 启动资源版本或布局不匹配，请先用配套一键包保留数据更新。'; return 1;
    }
}

check_data_volume() {
    set -- $(read_magic "$DARWIN_NODE" "$DATA_BLOCK" 1024 4 | od -An -v -tx1 2>/dev/null)
    [ "$*" = '48 58 00 05' ] || {
        switch_error '未找到已初始化的 iOS 数据卷，请先完成双系统安装。'; return 1;
    }
    set -- $(read_magic "$DARWIN_NODE" "$DATA_BLOCK" 1064 8 | od -An -v -tu1 2>/dev/null)
    [ "$#" = 8 ] || { switch_error '无法读取 iOS 数据卷容量。'; return 1; }
    volume_block_size=$(( $1 * 16777216 + $2 * 65536 + $3 * 256 + $4 ))
    volume_blocks=$(( $5 * 16777216 + $6 * 65536 + $7 * 256 + $8 ))
    [ "$volume_block_size" = 4096 ] && [ "$volume_blocks" = "$(( DARWIN_SIZE / 4096 - DATA_BLOCK ))" ] || {
        switch_error 'iOS 数据卷容量与实际分区不一致，请保留安装记录并检查，勿重复首次安装。'; return 1;
    }
}

check_target() {
    [ "$(id -u)" = 0 ] || { switch_error '需要由已取得 root 的管理器执行。'; return 1; }
    [ "$(getprop ro.product.device)" = "$TARGET_DEVICE" ] || {
        switch_error '机型不匹配，本模块仅适用于 Redmi K20 Pro / raphael。'; return 1;
    }
    [ "$(getprop sys.boot_completed)" = 1 ] || {
        switch_error '请等待 Android 完成开机后再操作。'; return 1;
    }
    RECOVERY_NODE=$(resolve_node recovery) || {
        switch_error '找不到 recovery 分区，请检查 root 权限和分区节点。'; return 1;
    }
    DARWIN_NODE=$(resolve_node darwin) || {
        switch_error '找不到 darwin 分区，请先完成双系统安装。'; return 1;
    }
    recovery_size=$(blockdev --getsize64 "$RECOVERY_NODE" 2>/dev/null)
    DARWIN_SIZE=$(blockdev --getsize64 "$DARWIN_NODE" 2>/dev/null)
    [ "$recovery_size" = "$RECOVERY_BYTES" ] || {
        switch_error '无法读取 recovery，或其大小不是配套的 64 MiB。'; return 1;
    }
    is_unsigned "$DARWIN_SIZE" &&
        [ "$DARWIN_SIZE" -ge "$DARWIN_MIN_BYTES" ] && [ "$DARWIN_SIZE" -le "$DARWIN_MAX_BYTES" ] &&
        [ "$(( DARWIN_SIZE % 4096 ))" = 0 ] || {
        switch_error 'iOS 分区容量不可读取或超出支持范围；自选容量无需固定为 20 GiB。'; return 1;
    }
    switch_message '1/3 检查 iOS 引导器，请稍候…'
    actual=$(sha256sum "$RECOVERY_NODE" 2>/dev/null) || {
        switch_error '读取 recovery 失败，请检查 root 权限。'; return 1;
    }
    [ "${actual%% *}" = "$RECOVERY_SHA256" ] || {
        switch_error 'recovery 不是配套蜂窝适配版引导器，可能是旧版、TWRP 或官方入口；请先运行配套一键包的一键更新。'; return 1;
    }
    switch_message '2/3 检查本机 iOS 启动资源…'
    check_resources || return 1
    switch_message '3/3 检查持久化数据卷与自选容量…'
    check_data_volume || return 1
    reboot_available || { switch_error 'Android 标准重启程序不可用。'; return 1; }
    switch_message "检查通过：iOS 分区约 $(( DARWIN_SIZE / 1048576 )) MiB。"
}

request_recovery() {
    # Do not call BusyBox reboot: Android's explicit binary handles recovery requests.
    /system/bin/reboot recovery
}

switch_main() {
    case "${1-}" in
        ''|--check) ;;
        *) switch_error '仅支持直接切换或 --check 只读检查。'; return 1;;
    esac
    [ ! -e "$MODDIR/disable" ] && [ ! -e "$MODDIR/remove" ] || {
        switch_error '模块已停用或等待卸载，请先在管理器启用并按提示完成重启。'; return 1;
    }
    switch_message '红米双系统 · 进入 iOS'
    check_target || return 1
    if [ "${1-}" = --check ]; then
        switch_message '只读检查结束，没有请求重启。'
        return 0
    fi
    switch_message '正在请求进入 iOS，请先保存 Android 中的操作；无需再按音量键。'
    request_recovery || { switch_error 'Android 未接受重启请求，请保留上方错误；不会自动重试。'; return 1; }
}
