#!/usr/bin/env bash
# 免 sudo 安装 GCC 15 到用户目录，用于 EDGEDESK_WEB_MODE=embed（GCC 15+ #embed）。
#
# 只使用 curl + dpkg -x，不调用 dpkg -i，不写入 /usr，不改动系统默认 gcc/g++。
# 默认安装到 $HOME/opt/gcc-15；deb 缓存在 $PREFIX/debs（可离线重装）。
#
# 用法：
#   scripts/install-gcc15-local.sh [--prefix DIR] [--from-dir DIR] [--force]
#
#   --prefix DIR   安装前缀，默认 $GCC15_PREFIX 或 $HOME/opt/gcc-15
#   --from-dir DIR 从已下载的 deb 目录复制，而不是联网（适用于 PPA 清理旧版本后）
#   --force        忽略缓存，重新下载全部 deb
set -euo pipefail

PPA_BASE="https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu"
# 编译期包与运行时包分属该 PPA 的 gcc-15 / gcc-16 路径：
# gcc-15 是项目基线；该 PPA 当前只保留 gcc-16 版本的 libstdc++6 / libgcc-s1。
DEB_DIR_GCC15="pool/main/g/gcc-15"
DEB_DIR_GCC16="pool/main/g/gcc-16"

# 版本与 sha256 锁定（2026-10-16 实测），格式： 目录|文件名|sha256
LOCKED_DEBS=(
  "$DEB_DIR_GCC15|gcc-15-base_15.2.0-14ubuntu1~24~ppa1_amd64.deb|38391f0ec049e6027dfe39bb52ef41db54e9b991f3fa2712c338694bbe4cbdee"
  "$DEB_DIR_GCC15|cpp-15_15.2.0-14ubuntu1~24~ppa1_amd64.deb|919ffe337747375ade9ed321eceba4fcdb3d0c5cb990910db76941690cd935bc"
  "$DEB_DIR_GCC15|cpp-15-x86-64-linux-gnu_15.2.0-14ubuntu1~24~ppa1_amd64.deb|4c62888e375bd78fa043684d604c773a16515b39355b7dc4b86fd96bc9005b67"
  "$DEB_DIR_GCC15|gcc-15_15.2.0-14ubuntu1~24~ppa1_amd64.deb|53b49adc337851db5f5fe5e59760785c145f4c91233dc73179776acf8d60d091"
  "$DEB_DIR_GCC15|gcc-15-x86-64-linux-gnu_15.2.0-14ubuntu1~24~ppa1_amd64.deb|9c000fefaee084a86c3b996de12b350922b8023884ddf9332101cf18f6f08b05"
  "$DEB_DIR_GCC15|g++-15_15.2.0-14ubuntu1~24~ppa1_amd64.deb|04ed78ecdc03113243f29ded10f031e8e879afc93eee62a6f818339984472e5f"
  "$DEB_DIR_GCC15|g++-15-x86-64-linux-gnu_15.2.0-14ubuntu1~24~ppa1_amd64.deb|9bcecd032ea69d2b6b0185320c792404aca3895ca95d980987a912e7c888bbcc"
  "$DEB_DIR_GCC15|libgcc-15-dev_15.2.0-14ubuntu1~24~ppa1_amd64.deb|f46847965dd48c543cc81e3a2658d2322b14c79d7c2d3ac4c8d8d87c39f5be2d"
  "$DEB_DIR_GCC15|libstdc++-15-dev_15.2.0-14ubuntu1~24~ppa1_amd64.deb|8ccc2fb691735b2912a320b650eee663fbfb351600f1cb288a81ccede1e2aea2"
  "$DEB_DIR_GCC16|libgcc-s1_16-20260315-1ubuntu1~24~ppa1_amd64.deb|c4ed89f1c027dd8049039e6af5fa9e29b3fd15a8da7ab61749fabf565973dcc7"
  "$DEB_DIR_GCC16|libstdc++6_16-20260315-1ubuntu1~24~ppa1_amd64.deb|17f9117ad94f189b08a39f775cd92476f76d52b954899455e2872747f1627841"
)

PREFIX="${GCC15_PREFIX:-$HOME/opt/gcc-15}"
FROM_DIR=""
FORCE=0

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
info() { printf '%s\n' "$*"; }

usage() {
  # 打印文件头部的注释块，避免帮助文本与注释漂移。
  awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"
  exit "${1:-0}"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="${2:?--prefix 需要目录参数}"; shift 2 ;;
    --from-dir) FROM_DIR="${2:?--from-dir 需要目录参数}"; shift 2 ;;
    --force) FORCE=1; shift ;;
    -h|--help) usage 0 ;;
    *) die "未知参数：$1（使用 --help 查看用法）" ;;
  esac
done

[ "$(uname -m)" = "x86_64" ] || die "锁定的 deb 为 _amd64，当前架构 $(uname -m) 不支持"
[ "$(uname -s)" = "Linux" ] || die "仅支持 Linux 宿主"
for tool in curl dpkg sha256sum; do
  command -v "$tool" >/dev/null 2>&1 || die "缺少必需命令：$tool"
done
if [ -n "$FROM_DIR" ]; then
  [ -d "$FROM_DIR" ] || die "--from-dir 目录不存在：$FROM_DIR"
fi

DEB_CACHE="$PREFIX/debs"
ROOT="$PREFIX/root"
BIN_DIR="$ROOT/usr/bin"
mkdir -p "$DEB_CACHE"

sha256_of() { sha256sum "$1" | cut -d' ' -f1; }

fetch_one() {
  # $1=相对目录 $2=文件名 $3=期望 sha256
  local dir="$1" name="$2" want="$3" dest="$DEB_CACHE/$2"
  if [ "$FORCE" = 0 ] && [ -f "$dest" ]; then
    if [ "$(sha256_of "$dest")" = "$want" ]; then
      info "使用缓存：$name"
      return 0
    fi
    info "缓存校验失败，重新获取：$name"
    rm -f "$dest"
  fi
  if [ -n "$FROM_DIR" ]; then
    [ -f "$FROM_DIR/$name" ] || die "--from-dir 中缺少 $name"
    install -m 0644 "$FROM_DIR/$name" "$dest"
    info "复制自 --from-dir：$name"
  else
    info "下载：$name"
    curl -fsSL --retry 3 --connect-timeout 20 -o "$dest.part" "$PPA_BASE/$dir/$name" \
      || { rm -f "$dest.part"; die "下载失败：$PPA_BASE/$dir/$name（PPA 可能已移除该锁定版本，改用 --from-dir 传入已缓存的 deb）"; }
    mv "$dest.part" "$dest"
  fi
  local got
  got="$(sha256_of "$dest")"
  [ "$got" = "$want" ] || die "sha256 不匹配：$name（期望 $want，实际 $got）"
}

info "安装前缀：$PREFIX"
for entry in "${LOCKED_DEBS[@]}"; do
  IFS='|' read -r dir name want <<<"$entry"
  fetch_one "$dir" "$name" "$want"
done

info "解压到 $ROOT（dpkg -x，不需要 root）"
rm -rf "$ROOT"
mkdir -p "$ROOT"
for entry in "${LOCKED_DEBS[@]}"; do
  IFS='|' read -r _ name _ <<<"$entry"
  dpkg -x "$DEB_CACHE/$name" "$ROOT"
done

if [ ! -x "$BIN_DIR/gcc-15" ] || [ ! -x "$BIN_DIR/g++-15" ]; then
  die "解压结果缺少 gcc-15 / g++-15：$BIN_DIR"
fi

info "自检：C++17 + #embed 探针"
probe_dir="$(mktemp -d)"
trap 'rm -rf "$probe_dir"' EXIT
printf 'embed-probe' > "$probe_dir/asset.bin"
cat > "$probe_dir/probe.cpp" <<'CPP'
#include <cstddef>
#include <string>
#include <iostream>
namespace { const unsigned char kData[] = {
#embed "asset.bin" suffix(,)
0 }; }
int main() {
  const std::string s(reinterpret_cast<const char*>(kData), sizeof(kData) - 1);
  std::cout << "embed_ok size=" << s.size() << "\n";
  return s == "embed-probe" ? 0 : 1;
}
CPP
# 使用与项目一致的 -std=c++17 -Wall -Wextra -Wpedantic；#embed 会给出 GCC 扩展警告但不影响退出码。
( cd "$probe_dir" \
  && "$BIN_DIR/g++-15" -std=c++17 -Wall -Wextra -Wpedantic probe.cpp -o probe 2>probe.stderr \
  && ./probe >/dev/null ) || { cat "$probe_dir/probe.stderr" >&2; die "#embed 探针编译或运行失败"; }

info ""
info "GCC 15 就绪：$("$BIN_DIR/g++-15" --version | head -1)"
info "  C  编译器：$BIN_DIR/gcc-15"
info "  C++编译器：$BIN_DIR/g++-15"
info "  注意：解压根只提供带版本后缀的命令，系统 gcc/g++ 不会被覆盖"
info "  下一步：scripts/with-gcc15.sh all   （或在 CMake 中传入上面的编译器路径）"
