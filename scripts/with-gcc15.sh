#!/usr/bin/env bash
# 用 ~/opt/gcc-15 中的本地 GCC 15 配置 / 构建 / 测试 EdgeDesk。
#
# 适用场景：EDGEDESK_WEB_MODE=embed 需要 GCC 15+（见 docs/dev/gcc15-local-toolchain.md），
# 而系统默认 gcc/g++ 仍是 13。GCC 15 由 scripts/install-gcc15-local.sh 安装在用户目录。
#
# 用法：
#   scripts/with-gcc15.sh configure [preset] [额外 cmake 参数...]
#   scripts/with-gcc15.sh build     [preset] [-j N]
#   scripts/with-gcc15.sh test      [preset]
#   scripts/with-gcc15.sh all       [preset] [-j N]      # configure + build + test
#   scripts/with-gcc15.sh env                            # 打印可 source 的环境变量
#
# preset 默认 host-embed（宿主 glibc 验证）；release-embed 等发布预设改用 musl 交叉工具链，
# 不能用本脚本的宿主编译器配置。GCC15_PREFIX 可覆盖安装前缀（默认 $HOME/opt/gcc-15）。
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${GCC15_PREFIX:-$HOME/opt/gcc-15}"
BIN_DIR="$PREFIX/root/usr/bin"
CC="$BIN_DIR/gcc-15"
CXX="$BIN_DIR/g++-15"
PRESETS_FILE="$REPO_ROOT/CMakePresets.json"

usage() {
  # 打印文件头部的注释块，避免帮助文本与注释漂移。
  awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"
  exit "${1:-0}"
}

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
info() { printf '%s\n' "$*"; }

require_toolchain() {
  if [ ! -x "$CC" ] || [ ! -x "$CXX" ]; then
    die "未找到本地 GCC 15：$CC / $CXX
先执行：scripts/install-gcc15-local.sh
或用 GCC15_PREFIX=/path 指定其他安装前缀。"
  fi
}

# 从 CMakePresets.json 读取预设的 binaryDir，避免在脚本里重复维护 out/build 约定。
bin_dir_for_preset() {
  local preset="$1"
  python3 - "$PRESETS_FILE" "$preset" "$REPO_ROOT" <<'PY'
import json, os, sys
presets_file, name, source_dir = sys.argv[1], sys.argv[2], sys.argv[3]
try:
    data = json.load(open(presets_file, encoding="utf-8"))
except (OSError, ValueError) as exc:
    sys.exit(f"无法读取 {presets_file}: {exc}")
for preset in data.get("configurePresets", []):
    if preset.get("name") == name and "binaryDir" in preset:
        # 仅处理本项目实际使用的 ${sourceDir} 变量。
        print(preset["binaryDir"].replace("${sourceDir}", source_dir))
        sys.exit(0)
sys.exit(f"CMakePresets.json 中没有预设：{name}（可用 cmake --list-presets 查看）")
PY
}

cmd_env() {
  cat <<EOF
# 本地 GCC 15（docs/dev/gcc15-local-toolchain.md）
export GCC15_PREFIX="$PREFIX"
export CC="$CC"
export CXX="$CXX"
export PATH="$BIN_DIR:\$PATH"
# 注意：解压根只提供带版本后缀的命令（gcc-15/g++-15），系统 gcc/g++ 不受影响。
EOF
}

cmd_configure() {
  local preset="$1"; shift
  require_toolchain
  local cache
  cache="$(bin_dir_for_preset "$preset")/CMakeCache.txt"
  if [ -f "$cache" ]; then
    local pair key want cached
    for pair in "CMAKE_C_COMPILER:$CC" "CMAKE_CXX_COMPILER:$CXX"; do
      key="${pair%%:*}"; want="${pair#*:}"
      cached="$(sed -n "s/^$key:[A-Z]*=//p" "$cache")"
      if [ -n "$cached" ] && [ "$cached" != "$want" ]; then
        die "构建目录已用其他编译器配置：$key=$cached
CMake 允许在编译器 ID/版本一致时静默替换编译器路径，容易误用，因此这里直接拒绝。
请先删除构建目录后重试：
  rm -rf \"$(dirname "$cache")\""
      fi
    done
  fi
  info "配置预设 $preset：$("$CXX" --version | head -1)"
  cmake --preset "$preset" -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" "$@"
}

cmd_build() {
  local preset="$1"; shift
  local jobs=()
  if [ $# -gt 0 ]; then jobs=("$@"); fi
  local binary_dir
  binary_dir="$(bin_dir_for_preset "$preset")"
  [ -f "$binary_dir/CMakeCache.txt" ] \
    || die "构建目录未配置：$binary_dir。先执行 scripts/with-gcc15.sh configure $preset"
  info "构建预设 $preset"
  cmake --build --preset "$preset" "${jobs[@]}"
}

cmd_test() {
  local preset="$1"; shift
  local binary_dir
  binary_dir="$(bin_dir_for_preset "$preset")"
  [ -d "$binary_dir" ] || die "构建目录不存在：$binary_dir。先执行 scripts/with-gcc15.sh configure $preset"
  info "测试预设 $preset（$binary_dir）"
  ctest --test-dir "$binary_dir" --output-on-failure "$@"
}

cmd_all() {
  local preset="$1"; shift
  local jobs=()
  if [ $# -gt 0 ]; then jobs=("$@"); fi
  cmd_configure "$preset"
  cmd_build "$preset" "${jobs[@]}"
  cmd_test "$preset"
}

[ $# -ge 1 ] || usage 1
command="${1}"; shift
case "$command" in
  configure) cmd_configure "${1:-host-embed}" "${@:2}" ;;
  build)     cmd_build     "${1:-host-embed}" "${@:2}" ;;
  test)      cmd_test      "${1:-host-embed}" "${@:2}" ;;
  all)       cmd_all       "${1:-host-embed}" "${@:2}" ;;
  env)       cmd_env ;;
  -h|--help|help) usage 0 ;;
  *) die "未知子命令：$command（可用：configure build test all env）" ;;
esac
