#!/usr/bin/env bash
# 构建 Codex Pet Bridge 菜单栏应用。
#
#   ./tools/menubar/build.sh                    构建, 产物在 build/menubar/
#   ./tools/menubar/build.sh --run              构建后直接启动(跑 build/menubar 那份)
#   ./tools/menubar/build.sh --install          构建 + 装到 /Applications + 重启
#   ./tools/menubar/build.sh --install-to <目录> 同上, 换个安装目录
#
# 只用 Command Line Tools 里的 swiftc, 不需要 Xcode 工程, 也不引入任何第三方依赖。
#
# 仓库位置**只在这个脚本里有意义**: 它用仓库去取源码、取固件头文件里的版本号和端口,
# 并把桥接脚本复制进应用包。应用本身不读源码目录 —— 装到别人机器上没有那个目录,
# 它靠的就是这里定好的东西。所以改完 `tools/pet_bridge.py` 或 `PET_BRIDGE_PORT`,
# 要重新跑这个脚本(装了的话用 --install, 一条命令连换装带重启)。
set -euo pipefail

usage() {
    cat <<'USAGE'
构建 Codex Pet Bridge 菜单栏应用。

  ./tools/menubar/build.sh                     构建, 产物在 build/menubar/
  ./tools/menubar/build.sh --run               构建后直接启动(跑 build/menubar 那份)
  ./tools/menubar/build.sh --install           构建 + 装到 /Applications + 重启
  ./tools/menubar/build.sh --install-to <目录>  同上, 换个安装目录

只用 Command Line Tools 里的 swiftc, 不需要 Xcode 工程, 也不引入任何第三方依赖。

仓库位置**只在这个脚本里有意义**: 它用仓库去取源码、取固件头文件里的版本号和端口,
并把桥接脚本复制进应用包。应用本身不读源码目录 —— 装到别人机器上没有那个目录,
它靠的就是这里定好的东西。所以改完 `tools/pet_bridge.py` 或 `PET_BRIDGE_PORT`,
要重新跑这个脚本(装了的话用 --install, 一条命令连换装带重启)。
USAGE
}

die() {
    echo "错误: $*" >&2
    exit 1
}

run_after=false
install_target=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --run) run_after=true; shift ;;
        --install) install_target="/Applications"; shift ;;
        --install-to)
            [[ $# -ge 2 ]] || die "--install-to 需要一个目录参数"
            install_target="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "未知参数: $1（用 --help 看用法）" ;;
    esac
done

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$here/../.." && pwd)"

app_name="CodexPetBridge"
app_display="Codex Pet Bridge"
out_root="$repo_root/build/menubar"
app_dir="$out_root/$app_display.app"
work_dir="$out_root/obj"

if ! command -v xcrun >/dev/null 2>&1 || ! xcrun --find swiftc >/dev/null 2>&1; then
    echo "找不到 swiftc。请安装 Xcode Command Line Tools: xcode-select --install" >&2
    exit 1
fi

# 版本号与桥接端口都沿用固件的 pet_config.h, 免得同一样东西两处各记一个。
# 端口必须是**编译期**常量: 应用运行时不读源码目录, 没有 pet_config.h 可读。
pet_config="$repo_root/main/pet_config.h"
version="$(sed -n 's/^#define[[:space:]]*PET_FIRMWARE_VERSION[[:space:]]*"\(.*\)".*/\1/p' \
    "$pet_config" | head -1)"
version="${version:-0.1.0}"
bridge_port="$(sed -n 's/^#define[[:space:]]*PET_BRIDGE_PORT[[:space:]]*\([0-9][0-9]*\).*/\1/p' \
    "$pet_config" | head -1)"
bridge_port="${bridge_port:-8765}"
[[ "$bridge_port" =~ ^[0-9]+$ ]] && (( bridge_port > 0 && bridge_port < 65536 )) \
    || die "从 $pet_config 读到的 PET_BRIDGE_PORT 不合法: $bridge_port"

rm -rf "$app_dir"
mkdir -p "$app_dir/Contents/MacOS" "$app_dir/Contents/Resources" "$work_dir"

cat > "$work_dir/BuildConfig.swift" <<EOF
// 由 tools/menubar/build.sh 生成, 不要手改。
//
// 刻意**没有**仓库路径: 应用运行时不读源码目录, 装到别人机器上也不需要它。
enum BuildConfig {
    static let version = "$version"
    static let bridgePort = $bridge_port
}
EOF

sources=()
for file in "$here"/Sources/*.swift; do
    sources+=("$file")
done

echo "编译 $app_display $version (桥接端口: $bridge_port, 源码: $repo_root)"
xcrun swiftc -O -swift-version 5 \
    -target "$(uname -m)-apple-macos12.0" \
    -o "$app_dir/Contents/MacOS/$app_name" \
    "${sources[@]}" "$work_dir/BuildConfig.swift" \
    -framework AppKit \
    -framework CoreBluetooth

cp "$here/Info.plist" "$app_dir/Contents/Info.plist"

# 把桥接脚本放进应用包: 这是应用唯一会去取脚本的地方 —— 它不会回头看仓库。
#
# 是**构建期复制**, 不是往仓库里再放一份副本: 单一来源仍然是 tools/ 下那一份。
# 两个文件必须同一个目录, 因为 pet_bridge.py 靠 sys.path[0] 找 gen_pet_package。
bridge_dir="$app_dir/Contents/Resources/bridge"
mkdir -p "$bridge_dir"
cp "$repo_root/tools/pet_bridge.py" "$repo_root/tools/gen_pet_package.py" "$bridge_dir/"

# 菜单栏图标: Assets/ 下 1x/2x 两档由同目录的 CodexPetBridge.png 派生(裁透明边、
# 阈值 64 过滤隐形像素、补方形 + 6% 边距、18/36px)。源图只留档, 运行时只用这两张;
# 换图标时用 tools 里的 Pillow 流程重新派生, 不要手动改这两张小图。
cp "$here/Assets/menu-icon.png" "$here/Assets/menu-icon@2x.png" \
    "$app_dir/Contents/Resources/"

# 本地构建的应用没有开发者证书; ad-hoc 签名足够让它正常运行(尤其是 Apple Silicon
# 要求可执行文件必须有签名)。
if ! codesign --force --sign - "$app_dir" >/dev/null 2>&1; then
    echo "提示: ad-hoc 签名失败, 应用可能仍可运行" >&2
fi

echo "完成: $app_dir (桥接脚本已内嵌: bridge/)"

# ---------------------------------------------------------------- 安装

if [[ -n "$install_target" ]]; then
    [[ -d "$install_target" ]] || die "安装目录不存在: $install_target"
    [[ -w "$install_target" ]] || die "安装目录不可写: $install_target（换一个, 或用 sudo）"

    target="$install_target/$app_display.app"
    bundle_id="$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" \
        "$here/Info.plist" 2>/dev/null || true)"
    [[ -n "$bundle_id" ]] || die "从 Info.plist 读不到 CFBundleIdentifier"

    # 目标已存在时先确认它确实是本应用。少了这一步, 一个写错的目录参数就可能删掉
    # 别人的 .app —— 在这个脚本里 rm -rf 是必须的, 那就得先证明删的是自己的东西。
    if [[ -e "$target" ]]; then
        existing_id="$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" \
            "$target/Contents/Info.plist" 2>/dev/null || true)"
        [[ "$existing_id" == "$bundle_id" ]] \
            || die "$target 已存在, 但它不是本应用(CFBundleIdentifier=${existing_id:-读不到})。请自行处理后再装。"
    fi

    # SIGTERM 会跳过 AppKit 的退出流程, 被监管的桥接子进程会变成孤儿继续占着端口,
    # 换装后新实例就起不来了。所以应用与子进程要分别收掉。
    # (注意: 这里按命令行匹配 pet_bridge.py, 会一并收掉手工启动的那个。)
    pkill -f "$app_name" 2>/dev/null || true
    sleep 1
    pkill -f "pet_bridge.py" 2>/dev/null || true
    sleep 1
    rm -f "$HOME/Library/Application Support/CodexPetBridge/bridge.sock"

    rm -rf "$target"
    cp -R "$app_dir" "$target"
    echo "已安装: $target"

    if [[ "$install_target" == "/Applications" ]]; then
        echo "提示: ad-hoc 签名的应用首次运行时, 蓝牙配网会弹权限询问, 需要有人点同意。"
    fi

    open "$target"
    echo "已重启。图标会出现在菜单栏右侧; 没有 Dock 图标是正常的。"

elif [[ "$run_after" == true ]]; then
    open "$app_dir"
    echo "已启动。图标会出现在菜单栏右侧; 没有 Dock 图标是正常的。"
fi
