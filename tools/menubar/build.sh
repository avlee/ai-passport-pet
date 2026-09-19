#!/usr/bin/env bash
# 构建 Codex Pet Bridge 菜单栏应用。
#
#   ./tools/menubar/build.sh          构建, 产物在 build/menubar/
#   ./tools/menubar/build.sh --run    构建后直接启动
#
# 只用 Command Line Tools 里的 swiftc, 不需要 Xcode 工程, 也不引入任何第三方依赖。
# 编译期会把"当前项目目录"写进 BuildConfig.swift 作为默认值, 所以本地构建出来的
# 应用开箱就指向这个仓库(之后可在菜单里改)。
set -euo pipefail

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

# 版本号沿用固件里的项目版本, 免得两处各记一个。
version="$(sed -n 's/^#define[[:space:]]*PET_FIRMWARE_VERSION[[:space:]]*"\(.*\)".*/\1/p' \
    "$repo_root/main/pet_config.h" | head -1)"
version="${version:-0.1.0}"

rm -rf "$app_dir"
mkdir -p "$app_dir/Contents/MacOS" "$app_dir/Contents/Resources" "$work_dir"

cat > "$work_dir/BuildConfig.swift" <<EOF
// 由 tools/menubar/build.sh 生成, 不要手改。
enum BuildConfig {
    static let repoPath = "$repo_root"
    static let version = "$version"
}
EOF

sources=()
for file in "$here"/Sources/*.swift; do
    sources+=("$file")
done

echo "编译 $app_display $version (仓库: $repo_root)"
xcrun swiftc -O -swift-version 5 \
    -target "$(uname -m)-apple-macos12.0" \
    -o "$app_dir/Contents/MacOS/$app_name" \
    "${sources[@]}" "$work_dir/BuildConfig.swift" \
    -framework AppKit

cp "$here/Info.plist" "$app_dir/Contents/Info.plist"

# 本地构建的应用没有开发者证书; ad-hoc 签名足够让它正常运行(尤其是 Apple Silicon
# 要求可执行文件必须有签名)。
if ! codesign --force --sign - "$app_dir" >/dev/null 2>&1; then
    echo "提示: ad-hoc 签名失败, 应用可能仍可运行" >&2
fi

echo "完成: $app_dir"

if [[ "${1:-}" == "--run" ]]; then
    open "$app_dir"
    echo "已启动。图标会出现在菜单栏右侧; 没有 Dock 图标是正常的。"
fi
