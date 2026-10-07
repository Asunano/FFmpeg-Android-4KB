# FFmpeg-Android-4KB

Android 通用 FFmpeg 预编译仓库：官方源码 **零 patch**，自建构建层 + 通用 JNI 桥，GitHub Actions 自动跟随官方 release 更新。

## 为什么是这个仓库

- **4KB 页对齐**：ffmpeg-kit 等预编译 so 按 16KB 页链接，在 4KB 页设备（绝大多数现役 Android 机）上 `dlopen` 直接崩。本仓库产物 `max-page-size=4096`，**4KB/8KB/16KB 页设备全部兼容**（对齐要求是 so ≥ 系统页大小，向下兼容）。
- **通用 JNI 桥**：`RegisterNatives` 动态注册，C 侧不感知 Java 包名。不改一行 C 代码即可接入任何项目。
- **可复现**：构建参数全部在 `android/build.sh`，本地与 CI 产出一致。
- **跟随上游**：官方发 release 自动构建，不打 beta/commit 快照。

## 产物

Release 附件命名：`libffmpeg_bridge-<官方版本>-<档位>-<架构>.so`，如：

```
libffmpeg_bridge-n7.1.1-common-arm64.so
```

版本 tag：`<官方版本>-ufi<N>`（同官方版本二次构建递增 N），如 `n7.1.1-ufi1`。

## 档位

| 档位 | 体积约 | 内容 |
|---|---|---|
| `min` | 2.3MB | mp4/mkv + h264/hevc/mjpeg |
| `mid` | 3.0MB | + vp9/mpeg4 + avi/flv/ts |
| `full` | 3.2MB | + vp8/av1 |
| `common`（默认） | ~4MB | 主流+老格式全量解码（wmv/rmvb/vc1/prores/theora…）+ 音频转码（aac/flac）+ 软滤镜 |

`common` 档额外能力：音频 demuxer（wav/mp3/aac/flac/ogg 全解码）、muxer（mp4/adts/flac/matroska/wav）、filter（scale/crop/pad/rotate/fps/thumbnail/overlay/transpose/hflip/vflip/format）。

## 接入你的项目（三步）

1. **拷 `android/jni/FfmpegBridge.java`** 进项目，**只改第一行 package**。类名与方法签名勿动（签名与 C 侧注册表一一对应，改了就是 `UnsatisfiedLinkError`）。
2. **改 `android/jni/bridge_config.txt`**：`BRIDGE_CLASS=com/你的包名/FfmpegBridge`（与第 1 步一致）。或构建时传 `BRIDGE_CLASS=com/你的包名/FfmpegBridge bash android/build.sh ...`。
3. 下载对应档位 so，命名 `libffmpeg_bridge.so` 放进 `app/src/main/jniLibs/arm64-v8a/`，代码里直接 `FfmpegBridge.extractFrame(...)` 调用（类内已 `System.loadLibrary`）。

所有路径必须是**设备本地路径**（`/sdcard/...` 或 app 私有目录）。桥内只走 file 协议，不支持 `content://`——上层先落到临时文件。

## 能力清单（Java 方法 → 功能）

| 方法 | 功能 | 说明 |
|---|---|---|
| `extractFrame` | 抽帧存 JPEG | atSeconds<=0 取时长 40% 处 |
| `extractThumbnails` | 批量抽帧 | 多时间点一次调用 |
| `probeVideoInfo` | 视频元信息 JSON | 时长/分辨率/编码/帧率/码率 |
| `probeAudioInfo` | 音频元信息 JSON | 采样率/声道/码率 |
| `probeDurationSeconds` | 时长（秒） | 音视频通用 |
| `clipVideo` | 切片 | **stream copy**，秒级、零质量损失 |
| `remuxVideo` | 容器转换 | mkv↔mp4↔ts，stream copy |
| `extractAudio` | 音轨提取/转码 | `copy`\|`flac`\|`aac` |
| `applyFilter` | 单帧滤镜出图 | 任意滤镜链组合 |

**诚实的能力边界**：
- 不重写 ffmpeg CLI；每个入口是一条精封装管线
- **视频重编码不做**（需要 libx264/libx265 外部库，引入会让体积和许可复杂化）——视频侧只做 stream copy 的切片/换容器
- 音频重编码限 ffmpeg **原生**编码器 flac/aac；MP3 编码无原生实现（只有外部 libmp3lame），不提供
- `min/mid/full` 档未启用转码/滤镜组件时，相关入口返回 `ERR_UNSUPPORTED`，原因见 `lastError()`

## 本地构建

```bash
# 需要 Android NDK r27b/c（或设置 ANDROID_NDK_HOME）
git clone https://github.com/<你>/FFmpeg-Android-4KB.git
cd FFmpeg-Android-4KB
bash android/build.sh . /tmp/out common
# 产物：/tmp/out/libffmpeg_bridge.so + enabled-components.txt（组件清单）
```

环境变量：`ANDROID_NDK_HOME`（NDK 路径）、`API_LEVEL`（默认 31，按你的 minSdk 调）、`BRIDGE_CLASS`（覆盖桥配置）。

## CI 自动化

- **`release-build.yml`**：官方发 release → 自动按 common/min 两档 × arm64 构建 → 三重校验（4KB 对齐 / JNI_OnLoad 导出 / 组件清单存档）→ 自动打 tag `<版本>-ufi1` 并发 Release。
- **`manual-build.yml`**：手动触发，自选档位/架构/源码 ref。

上游 release 事件的 webhook 对 fork 有数小时延迟，属正常。

## 许可

FFmpeg 本体遵循其原始许可（LGPL/GPL 视 configure 而定，本仓库默认 **LGPL-2.1+**，未引入 GPL 组件）；`android/` 构建层与 JNI 桥同许可。商用注意：AAC 编码器在部分国家涉及专利许可，自行评估。
