#!/bin/bash
# ============================================================================
# FFmpeg-Android-4KB 通用构建脚本（Android arm64，4KB 页对齐，全页设备兼容）
#
# 在任何机器上复现 libffmpeg_bridge.so 的完整步骤：
#   1. 装 Android NDK（r27b/c 或兼容版本）
#   2. 准备 FFmpeg 源码目录（本仓库克隆即为源码树）
#   3. 运行本脚本：
#        android/build.sh <ffmpeg源码目录> <输出目录> [档位]
#      档位: min | mid | full | common  (默认 common)
#
# 通用性说明：
#   - 4KB 页对齐（max-page-size=4096）的 so 在 4KB/8KB/16KB 页设备上均可加载
#     （对齐要求是 so >= 系统页大小，向下兼容；反之 16KB 对齐 so 在 4KB 页设备
#      加载即崩，这正是本仓库存在的原因）
#   - JNI 桥经 RegisterNatives 动态注册，与 Java 包名零耦合：
#     使用方只需改 android/jni/bridge_config.txt 里的目标类全名
#
# NDK 路径优先取环境变量 ANDROID_NDK_HOME / ANDROID_NDK_ROOT，
# 未设置时依次探测常见安装路径。
# 产物: <输出目录>/libffmpeg_bridge.so
# ============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:?用法: $0 <ffmpeg源码目录> <输出目录> [min|mid|full|common]}"
OUT="${2:?用法: $0 <ffmpeg源码目录> <输出目录> [min|mid|full|common]}"
TIER="${3:-common}"

# ── NDK 定位 ──────────────────────────────────────────────────────────
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [ -z "$NDK" ]; then
  for c in "$HOME/android-ndk-r27c" /opt/android-sdk/ndk/27.2.12479018 \
           "$HOME/Library/Android/sdk/ndk/27.2.12479018" \
           "$HOME/Android/Sdk/ndk/27.2.12479018" \
           "${ANDROID_HOME:-/nonexistent}/ndk/27.2.12479018"; do
    [ -d "$c" ] && NDK="$c" && break
  done
fi
[ -d "$NDK" ] || { echo "找不到 NDK：请设置 ANDROID_NDK_HOME 指向 NDK 目录（推荐 r27c）"; exit 2; }
echo "使用 NDK: $NDK"

API="${API_LEVEL:-31}"                  # 默认 31，使用方按自己 minSdk 覆盖
ARCH=aarch64
TRIPLE=$ARCH-linux-android
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
[ -d "$TOOLCHAIN" ] || TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/darwin-x86_64
SYSROOT=$TOOLCHAIN/sysroot
CC=$TOOLCHAIN/bin/${TRIPLE}${API}-clang
AR=$TOOLCHAIN/bin/llvm-ar
RANLIB=$TOOLCHAIN/bin/llvm-ranlib
STRIP=$TOOLCHAIN/bin/llvm-strip
READELF=$TOOLCHAIN/bin/llvm-readelf
NM=$TOOLCHAIN/bin/llvm-nm
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu)

SO_NAME=libffmpeg_bridge.so

# ── 桥配置：目标 Java 类（bridge_config.txt 或环境变量 BRIDGE_CLASS 覆盖）──
if [ -z "$BRIDGE_CLASS" ]; then
  BRIDGE_CLASS=$(grep -E '^BRIDGE_CLASS=' "$SCRIPT_DIR/jni/bridge_config.txt" 2>/dev/null | cut -d= -f2- | tr -d '[:space:]')
fi
BRIDGE_CLASS="${BRIDGE_CLASS:-com/example/media/FfmpegBridge}"
echo "JNI 桥目标类: $BRIDGE_CLASS"

# ★ 4KB 页对齐（8KB/16KB 页设备向下兼容；16KB 对齐产物在 4KB 页设备会崩）
LDFLAGS_PAGE="-Wl,-z,max-page-size=4096 -Wl,-z,common-page-size=4096"

# ── 档位定义 ──────────────────────────────────────────────────────────
# min    ≈2.3MB  mp4/mkv + h264/hevc
# mid    ≈3.0MB  + vp9/mpeg4 + avi/flv/ts
# full   ≈3.2MB  + vp8/av1
# common ≈3.8MB  常用完整档：主流视频 + 老格式（mpeg1/2、wmv、rmvb、flv、vc1、dv、prores、theora）
# 说明：转码/滤镜入口（桥的 transcode/applyFilters）需要对应 encoder/filter，
#       common 档额外启用 aac/mp3 编码与部分软滤镜；min/mid/full 桥会回报告知不支持。
case "$TIER" in
  min)    DEM="mov,matroska";                                             DEC="h264,hevc,mjpeg";                          PAR="h264,hevc" ;;
  mid)    DEM="mov,matroska,avi,mpegts,flv,webm";                          DEC="h264,hevc,vp9,mpeg4,mjpeg";                PAR="h264,hevc,vp9,mpeg4video" ;;
  full)   DEM="mov,matroska,avi,mpegts,flv,webm";                          DEC="h264,hevc,vp8,vp9,av1,mpeg4,mjpeg";        PAR="h264,hevc,vp9,av1,mpeg4video" ;;
  common) DEM="mov,matroska,avi,mpegts,flv,webm,asf,rm,ogg,mpegps,mpegvideo,dv,image2"; \
          DEC="h264,hevc,vp8,vp9,av1,mpeg4,mjpeg,mpeg1video,mpeg2video,wmv1,wmv2,wmv3,flv1,vp6,vp6f,rv30,rv40,vc1,dvvideo,prores,theora,png"; \
          PAR="h264,hevc,vp9,av1,mpeg4video,mpegvideo,vc1" ;;
  *) echo "未知档位: $TIER（可选 min|mid|full|common）"; exit 2 ;;
esac

# common 档为转码/滤镜入口加料（音频编码、音频解复用、容器复用、软滤镜）
# 能力边界（诚实声明，写进 BUILD.md）：
#   视频重编码需要 libx264/libx265 等外部库，本仓库不引入 → 视频转码仅支持
#   「无重编码封装转换 / 切片」（stream copy）；音频重编码用 ffmpeg 原生编码器
#   （aac / flac），MP3 编码无原生实现（只有外部 libmp3lame），故不提供。
#   release/filter（drawtext）依赖 freetype，同样不引入。
if [ "$TIER" = "common" ]; then
  DEM="$DEM,wav,mp3,aac,flac,ogg"
  DEC="$DEC,aac,mp3,flac,opus,vorbis,ac3,eac3,dts,pcm_s16le,alac"
  EXTRA_ENC="--enable-encoder=aac,flac,pcm_s16le"
  EXTRA_MUX="--enable-muxer=mp4,adts,flac,matroska,wav"
  EXTRA_FILTER="--enable-filter=scale,format,crop,pad,rotate,fps,thumbnail,overlay,transpose,hflip,vflip"
  EXTRA_PROTOCOL="--enable-protocol=pipe"
else
  EXTRA_ENC=""; EXTRA_MUX=""; EXTRA_FILTER=""; EXTRA_PROTOCOL=""
fi

BUILD="$SRC/build-android-$ARCH-$TIER"
PREFIX=$BUILD/install
mkdir -p "$BUILD" "$OUT"

echo "== [1/4] configure (档位=$TIER) =="
cd "$SRC"
make distclean >/dev/null 2>&1 || true
./configure \
  --prefix="$PREFIX" \
  --enable-cross-compile --target-os=android --arch=$ARCH --cpu=armv8-a \
  --cc="$CC" --ar="$AR" --ranlib="$RANLIB" --strip="$STRIP" --sysroot="$SYSROOT" \
  --enable-static --disable-shared --enable-pic \
  --disable-everything \
  --disable-programs --disable-doc --disable-debug --disable-htmlpages \
  --disable-network --disable-autodetect \
  --enable-protocol=file $EXTRA_PROTOCOL \
  --enable-demuxer="$DEM" \
  --enable-decoder="$DEC" \
  --enable-parser="$PAR" \
  --enable-encoder=mjpeg $EXTRA_ENC \
  --enable-muxer=image2 $EXTRA_MUX \
  --enable-swscale \
  $EXTRA_FILTER \
  --enable-small \
  --extra-cflags="-Oz -fPIC -ffunction-sections -fdata-sections" \
  --extra-ldflags="$LDFLAGS_PAGE -Wl,--gc-sections" \
  > "$BUILD/configure.log" 2>&1 || { tail -30 "$BUILD/configure.log"; exit 1; }

echo "== [2/4] make ($JOBS jobs) =="
make -j$JOBS > "$BUILD/make.log" 2>&1 || { tail -30 "$BUILD/make.log"; exit 1; }
make install > "$BUILD/install.log" 2>&1

echo "== [3/4] 链接单文件 .so（FFmpeg 静态库 + 通用 JNI 桥）=="
# --exclude-libs,ALL / -Bsymbolic：修复 FFmpeg 7.x/8.x aarch64 NEON 汇编 PIC 重定位
# （ff_tx_tab_*_float 全局符号导致的 ADR_PREL_PG_HI21 报错），缺了必然链接失败
$CC -shared -o "$OUT/$SO_NAME" \
  "$SCRIPT_DIR/jni/ffmpeg_bridge.c" \
  -I"$SRC" -I"$PREFIX/include" \
  -DBRIDGE_CLASS="\"$BRIDGE_CLASS\"" \
  -L"$PREFIX/lib" \
  -Wl,--exclude-libs,ALL -Wl,-Bsymbolic \
  -lavformat -lavcodec -lavutil -lswscale -lswresample -lavfilter \
  -lm -lz \
  -Wl,-soname,$SO_NAME \
  $LDFLAGS_PAGE \
  -Wl,--gc-sections -O2

$STRIP --strip-unneeded "$OUT/$SO_NAME"

echo "== [4/4] 校验 =="
# 1) 4KB 页对齐
if $READELF -lW "$OUT/$SO_NAME" | awk '/LOAD/ {gsub(/0x/,"",$NF); if (strtonum("0x"$NF) > 4096) bad=1} END {exit bad?1:0}'; then
  echo "OK: 全部 LOAD 段 Align <= 4096（4KB/8KB/16KB 页设备通用）"
else
  echo "FAIL: 存在 Align > 4096 的 LOAD 段（4KB 页设备会 RELRO 崩溃）"
  $READELF -lW "$OUT/$SO_NAME" | grep LOAD; exit 1
fi
# 2) JNI_OnLoad 导出（RegisterNatives 桥的唯一门面）
$NM -D "$OUT/$SO_NAME" | grep -q "T JNI_OnLoad" || { echo "FAIL: JNI_OnLoad 未导出"; exit 1; }
echo "OK: JNI_OnLoad 已导出（RegisterNatives 动态注册，无包名耦合）"
# 3) 记录启用的解码器/编码器清单（可追溯）
grep -E "^#define (CONFIG_)?(DEMUXER|DECODER|ENCODER|MUXER|FILTER)" "$SRC/config.h" 2>/dev/null | grep -v " 0$" > "$OUT/enabled-components.txt" || true
echo "组件清单 → $OUT/enabled-components.txt"

SIZE=$(stat -c%s "$OUT/$SO_NAME" 2>/dev/null || stat -f%z "$OUT/$SO_NAME")
echo "== 体积: $SIZE bytes ($(( SIZE / 1024 )) KB)  [档位=$TIER] =="
echo "OK: 产物 → $OUT/$SO_NAME"
