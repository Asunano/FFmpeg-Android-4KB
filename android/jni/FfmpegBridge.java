/*
 * FFmpeg-Android-4KB —— Java 侧桥接模板
 *
 * 使用：拷进自己项目，只改 package 行（类名/方法签名勿动），同时把
 * android/jni/bridge_config.txt 的 BRIDGE_CLASS 改成相同全名，重新构建 so。
 * 方法签名与 C 侧 JNINativeMethod 表一一对应，**签名改动 = UnsatisfiedLinkError**。
 *
 * 所有路径都是设备本地路径（/sdcard/... 或 app 私有目录）。桥内只走
 * ffmpeg 的 file 协议，不支持 content:// URI —— 上层先落到临时文件再传路径。
 */
package com.example.media;

public final class FfmpegBridge {
    static { System.loadLibrary("ffmpeg_bridge"); }

    private FfmpegBridge() {}

    /** 抽一帧存 JPEG。atSeconds<=0 取时长 40% 处；maxSize<=0 不缩放。返回 0 成功，负数失败。 */
    public static native int extractFrame(String srcPath, String dstPath, double atSeconds, int maxSize);

    /**
     * 批量抽帧：times 为秒数组，输出 outDir/prefix_<i>.jpg。
     * 返回成功张数（失败原因见 lastError()）。
     */
    public static native int extractThumbnails(String srcPath, String outDir, String prefix, double[] times, int maxSize);

    /** 视频元信息 JSON：duration_s/width/height/codec/pix_fmt/bit_rate/fps_num/fps_den。失败返回 null。 */
    public static native String probeVideoInfo(String srcPath);

    /** 音频元信息 JSON：duration_s/codec/sample_rate/channels/bit_rate。失败返回 null。 */
    public static native String probeAudioInfo(String srcPath);

    /** 时长（秒）。<=0 未知。音视频通用。 */
    public static native double probeDurationSeconds(String srcPath);

    /** 无重编码切片（stream copy，秒级，零质量损失）。要求 endSec>startSec。 */
    public static native int clipVideo(String srcPath, String dstPath, double startSec, double endSec);

    /** 容器转换（stream copy）：mkv<->mp4<->ts 等，按 dstPath 后缀推断容器。 */
    public static native int remuxVideo(String srcPath, String dstPath);

    /**
     * 音轨提取/转码。codec："copy"（原样打包）| "flac" | "aac"（重编码，common 档才有）。
     * dstPath 后缀决定容器（.m4a/.aac/.flac）。MP3 编码不可用（ffmpeg 无原生 mp3 encoder）。
     */
    public static native int extractAudio(String srcPath, String dstPath, String codec);

    /**
     * 单帧滤镜出图：解码到 atSeconds → filterGraph → JPEG（maxSize 缩放）。
     * graph 例："scale=640:-1,transpose=1"。可用滤镜受档位限制（见 BUILD.md）。
     */
    public static native int applyFilter(String srcPath, String dstPath, String filterGraph, double atSeconds, int maxSize);

    /** 最近一次失败的文本原因（调试用）。 */
    public static native String lastError();

    /** 桥版本 + libav 版本串。 */
    public static native String bridgeVersion();
}
