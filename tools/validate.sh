#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

# 临时目录统一走 TMPDIR(macOS 上是 /var/folders/... ,不是 /tmp), 并去掉结尾的
# 斜杠: macOS 的 TMPDIR 以 "/" 结尾, 直接拼出 "//" 会让后续 abspath 规范化成
# 另一个字符串, 在按路径做访问控制的沙箱里会被判成"路径不认识"。
scratch_root="${TMPDIR:-/tmp}"
scratch_root="${scratch_root%/}"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

# 挑一个能真正跑"图集 -> .pet"的解释器。PET_PYTHON 给定了就照用, 不再探测 ——
# 显式指定的东西不该被悄悄换掉。
pick_pet_python() {
    local candidate
    if [[ -n "${PET_PYTHON:-}" ]]; then
        printf '%s' "${PET_PYTHON}"
        return 0
    fi
    # python3 走 PATH: 激活过的 venv 本来就会被它命中, 所以最常见的情况在第一个候选
    # 就解决了。后面几个是没激活 venv 时的常见落点。都不行就用 PET_PYTHON 指定。
    for candidate in python3 .venv/bin/python3 "${HOME:-}/.venv/bin/python3" \
        /opt/homebrew/bin/python3 /usr/local/bin/python3; do
        if command -v "${candidate}" >/dev/null 2>&1 &&
            "${candidate}" -c 'import PIL' >/dev/null 2>&1; then
            printf '%s' "${candidate}"
            return 0
        fi
    done
    printf 'python3'
}

run_static_checks() {
    local actionlint_bin
    local pet_python
    local test_dir

    python3 tools/check_repo.py

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    "${actionlint_bin}" -color .github/workflows/*.yml

    test_dir="$(mktemp -d "${scratch_root}/ai-passport-host-tests.XXXXXX")"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    "${test_dir}/test_ui_pixel_math"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_demo_navigation.c main/demo_navigation.c \
        -o "${test_dir}/test_demo_navigation"
    "${test_dir}/test_demo_navigation"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_display_rounding"
    "${test_dir}/test_bsp_display_rounding"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_es8311_sleep_check"
    "${test_dir}/test_bsp_es8311_sleep_check"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_button.c -o "${test_dir}/test_bsp_button"
    "${test_dir}/test_bsp_button"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_lvgl_init"
    "${test_dir}/test_bsp_lvgl_init"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_audio_recovery"
    "${test_dir}/test_bsp_audio_recovery"
    for demo in audio low_power ble wifi; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
            -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
            "tests/test_demo_${demo}_runtime.c" -Wl,--gc-sections \
            -o "${test_dir}/test_demo_${demo}_runtime"
        "${test_dir}/test_demo_${demo}_runtime"
    done

    # Codex 宠物: 状态机 / 协议解析 / 图集自检都是纯逻辑, 直接在主机上跑。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_pet_state.c main/pet_state.c \
        -o "${test_dir}/test_pet_state"
    "${test_dir}/test_pet_state"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_pet_protocol.c main/pet_protocol.c main/pet_state.c \
        -o "${test_dir}/test_pet_protocol"
    "${test_dir}/test_pet_protocol"
    # 宠物包格式: 帧表/状态表的布局一旦写错, 表现是"动画错位"或"花屏", 全都能正常
    # 编译运行 —— 所以格式的每一条约束都在主机上单独钉一遍。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_pet_pkg.c main/pet_pkg.c main/pet_state.c \
        -o "${test_dir}/test_pet_pkg"
    "${test_dir}/test_pet_pkg"
    # 版式: 舞台尺寸现在是运行时的, 算错的后果同样是静默的(宠物整体偏几像素)。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_pet_layout.c main/pet_layout.c \
        -o "${test_dir}/test_pet_layout"
    "${test_dir}/test_pet_layout"
    # 预览工具把同一套版式算术在 Python 里又写了一遍(它编译不了 C), 两份实现跑偏
    # 只会让预览图骗人 —— 拿上面的 C 逐字段对照。不需要 Pillow。
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_pet_layout_mirror.py
    # 蓝牙配网: 命令解析与状态拼装是纯字符串逻辑。这一层的错误只会表现成"界面一直
    # 停在等待配对", 所以在主机上把边界和拒绝路径钉死。
    # -Itests/host_stubs 给出 pet_settings.h 需要的 esp_err.h 替身(只在这一条命令里,
    # 不会影响固件编译)。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain -Itests/host_stubs \
        tests/test_pet_provision.c main/pet_provision_parse.c \
        -o "${test_dir}/test_pet_provision"
    "${test_dir}/test_pet_provision"
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_pet_font_coverage.py
    # 字库覆盖只保证"不缺字", 不保证"放得下": 配网状态区只有 200px, 文案长几个像素
    # 就会折行。这条用与 LVGL 相同的算法量宽度。
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_pet_ui_text_fit.py
    # 配网页写着"长按下键退出配网", 而那条分支曾经只开不关 —— 用户按提示去关却关不掉,
    # 满屏被配网页盖住, 看起来就是死机。这条把提示语和按键处理钉在一起。
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_pet_button_semantics.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_pet_bridge.py
    # 宠物包是跨语言契约(Python 写字节, C 按结构体读), 错开了只会静默拒收。
    # 这条让生成器真正写出来的字节走一遍设备解析器, 并逐字节确认三条 CRC 各自
    # 守住了范围。
    #
    # 解释器要**优先挑带 Pillow 的**: 换宠物第一步是"图集 -> .pet", 只有那几个
    # 模块用得到 Pillow。拿一个没装 Pillow 的 python3 硬跑也能过 —— 它会跳过整条
    # 打包管线, 于是门禁全绿而那段代码一行都没被执行到。真机上就撞过一次:
    # 桥接被 GUI 用系统 python 拉起, 启动一切正常, 只有换宠物报"内部错误"。
    # PET_PYTHON 可以显式指定(给了就照用, 不再探测)。
    pet_python="$(pick_pet_python)"
    if "${pet_python}" -c 'import PIL' >/dev/null 2>&1; then
        echo "[host] 宠物打包用 ${pet_python}（有 Pillow）"
    else
        echo "[host] 没找到带 Pillow 的解释器, '图集 -> .pet' 那一段会被跳过"
    fi
    PYTHONDONTWRITEBYTECODE=1 "${pet_python}" tests/test_pet_package.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_deep_sleep_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_repo.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_archive_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_install_passport_skills.py
    rm -rf "${test_dir}"
    echo "Host tests: PASS"
}

run_firmware_checks() (
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    validation_build_dir="$(mktemp -d "${scratch_root}/ai-passport-firmware.XXXXXX")"
    trap 'case "${validation_build_dir}" in */ai-passport-firmware.*) rm -rf -- "${validation_build_dir}" ;; esac' EXIT

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${validation_build_dir}"
    PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
        "${validation_build_dir}" --archive-root "${repo_root}/build/firmware"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    echo "Firmware build: PASS"
)

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac
