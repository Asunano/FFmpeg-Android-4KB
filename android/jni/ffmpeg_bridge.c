/*
 * FFmpeg-Android-4KB —— 通用 JNI 桥（RegisterNatives 动态注册，与包名零耦合）
 *
 * ## 为什么用 RegisterNatives
 * 传统 JNI 导出函数名必须编码 Java 包名（com.ufi_axis_core →
 * Java_com_ufi_1axis_1core_...），包名含 `_` 时还要 `_1` 转义——这是实际踩过的坑
 * （UnsatisfiedLinkError: No implementation found，符号永远匹配不上）。
 * RegisterNatives 在 JNI_OnLoad 里把 C 函数指针映射到类方法，C 侧完全不知道
 * Java 包名，同一个 .so 可被任何项目复用。
 *
 * ## 使用方接入（三步）
 *   1. 拷 android/jni/FfmpegBridge.java 到自己项目，只改 package 行
 *   2. 把 android/jni/bridge_config.txt 的 BRIDGE_CLASS 改成同一个全名
 *      （或构建时 -DBRIDGE_CLASS='"com/example/media/FfmpegBridge"' 覆盖）
 *   3. System.loadLibrary("ffmpeg_bridge") 后直接调用
 *
 * ## 能力面（Java native 方法签名固定，改了 = UnsatisfiedLinkError）
 *   extractFrame        抽帧存 JPEG
 *   probeVideoInfo      视频元信息 JSON
 *   probeAudioInfo      音频元信息 JSON
 *   probeDurationSeconds 时长（音视频通用）
 *   extractThumbnails   批量抽帧（多时间点）
 *   clipVideo           无重编码切片（stream copy，秒级）
 *   remuxVideo          容器转换（stream copy）
 *   extractAudio        音轨提取/重编码（copy|flac|aac）
 *   applyFilter         单帧滤镜出图（任意滤镜链）
 *   lastError / bridgeVersion
 *
 * ## 诚实的能力边界
 *   - 不重写 ffmpeg CLI；每个入口是一条精封装管线
 *   - 视频重编码需 libx264/libx265 外部库，本仓库不引入 → 视频侧只做
 *     stream copy（clip/remux）；音频重编码限 ffmpeg 原生 flac/aac
 *   - 未启用的 encoder/filter 返回 ERR_UNSUPPORTED，原因写入 lastError
 *
 * 全部按 4KB 页对齐链接（见 build.sh），4KB/8KB/16KB 页设备通用。
 */

#include <jni.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#ifndef BRIDGE_CLASS
#define BRIDGE_CLASS "com/example/media/FfmpegBridge"
#endif

#define OK 0
#define ERR_OPEN -1
#define ERR_STREAM -2
#define ERR_DECODER -3
#define ERR_SEEK -4
#define ERR_DECODE -5
#define ERR_SCALE -6
#define ERR_ENCODER -7
#define ERR_OUTPUT -8
#define ERR_PARAM -9
#define ERR_FILTER -10
#define ERR_MUX -11
#define ERR_UNSUPPORTED -12

static char g_err[512];

static void set_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
}

/* av_err2str 展开需要局部 char 缓冲，直接作实参不安全，包一层 */
static const char *errstr(int e) {
    static char buf[256];
    av_strerror(e, buf, sizeof(buf));
    return buf;
}

/* ── 公共：打开输入 ─────────────────────────────────────────────── */
static int open_input(const char *path, AVFormatContext **fmt_out) {
    AVFormatContext *fmt = NULL;
    int ret = avformat_open_input(&fmt, path, NULL, NULL);
    if (ret < 0) { set_err("打开失败: %s", errstr(ret)); return ERR_OPEN; }
    ret = avformat_find_stream_info(fmt, NULL);
    if (ret < 0) { set_err("读流信息失败: %s", errstr(ret)); avformat_close_input(&fmt); return ERR_STREAM; }
    *fmt_out = fmt;
    return OK;
}

static int open_video(const char *path, AVFormatContext **fmt_out, int *vidx_out) {
    int r = open_input(path, fmt_out);
    if (r != OK) return r;
    int vidx = av_find_best_stream(*fmt_out, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vidx < 0) { set_err("没有视频流"); avformat_close_input(fmt_out); return ERR_STREAM; }
    *vidx_out = vidx;
    return OK;
}

/* ── 解码：定位到 target 秒（<=0 取 40% 处，与既有口径一致）─────────
 * 返回的 AVFrame 由调用方 av_frame_free。*/
static AVFrame *decode_to_time(AVFormatContext *fmt, int vidx, double target, int *err_out) {
    AVStream *vs = fmt->streams[vidx];
    const AVCodec *dec = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!dec) { set_err("未编译该编码的解码器 codec_id=%d", vs->codecpar->codec_id); *err_out = ERR_DECODER; return NULL; }
    AVCodecContext *dec_ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dec_ctx, vs->codecpar);
    dec_ctx->thread_count = 1; /* 低内存设备单线程解码，避免峰值 */
    int ret = avcodec_open2(dec_ctx, dec, NULL);
    if (ret < 0) {
        set_err("打开解码器失败: %s", errstr(ret));
        avcodec_free_context(&dec_ctx); *err_out = ERR_DECODER; return NULL;
    }

    double duration = (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0)
                      ? fmt->duration / (double)AV_TIME_BASE : 0.0;
    if (target <= 0.0) target = duration > 0 ? duration * 0.40 : 10.0;
    if (duration > 0 && target > duration) target = duration * 0.95;

    int64_t seek_ts = (int64_t)(target * AV_TIME_BASE);
    ret = av_seek_frame(fmt, -1, seek_ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) set_err("seek 失败（非致命，顺序解）: %s", errstr(ret));

    AVFrame *frame = av_frame_alloc();
    AVFrame *best = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    int got = 0;
    int budget = 600; /* 上限：损坏文件防死循环 */

    while (!got && budget-- > 0 && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != vidx) { av_packet_unref(pkt); continue; }
        ret = avcodec_send_packet(dec_ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;
        while (avcodec_receive_frame(dec_ctx, frame) == 0) {
            int64_t pts_us = frame->best_effort_timestamp;
            double pts_s = (pts_us == AV_NOPTS_VALUE) ? -1.0 : pts_us * av_q2d(vs->time_base);
            /* 保存为"最后一帧"（解到目标前以备目标不可达时兜底） */
            av_frame_unref(best);
            av_frame_move_ref(best, frame);
            if (pts_s < 0.0 || pts_s + 0.001 >= target) { got = 1; break; }
        }
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&dec_ctx);

    if (best->width <= 0) { set_err("解码没有产出画面"); av_frame_free(&best); *err_out = ERR_DECODE; return NULL; }
    return best;
}

/* 缩放到 maxSize 内（宽高取偶，JPEG 要求）。返回的 frame 由调用方 free。*/
static AVFrame *scale_frame(AVFrame *src, int maxSize) {
    int w = src->width, h = src->height;
    if (maxSize > 0 && (w > maxSize || h > maxSize)) {
        if (w >= h) { h = h * maxSize / w; w = maxSize; }
        else        { w = w * maxSize / h; h = maxSize; }
    }
    w &= ~1; h &= ~1;
    if (w < 2) w = 2;
    if (h < 2) h = 2;
    AVFrame *scaled = av_frame_alloc();
    scaled->format = AV_PIX_FMT_YUVJ420P;
    scaled->width = w; scaled->height = h;
    if (av_frame_get_buffer(scaled, 32) < 0) { set_err("分配缩放帧失败"); av_frame_free(&scaled); return NULL; }
    struct SwsContext *sws = sws_getContext(src->width, src->height, src->format,
                                            w, h, AV_PIX_FMT_YUVJ420P, SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) { set_err("sws_getContext 失败（格式转换不支持）"); av_frame_free(&scaled); return NULL; }
    sws_scale(sws, (const uint8_t * const *)src->data, src->linesize, 0, src->height,
              scaled->data, scaled->linesize);
    sws_freeContext(sws);
    return scaled;
}

/* 把一帧编码成 JPEG 写文件 */
static int write_jpeg(AVFrame *scaled, const char *dst) {
    const AVCodec *enc = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!enc) { set_err("未编译 mjpeg 编码器"); return ERR_UNSUPPORTED; }
    AVCodecContext *enc_ctx = avcodec_alloc_context3(enc);
    enc_ctx->width = scaled->width; enc_ctx->height = scaled->height;
    enc_ctx->time_base = (AVRational){1, 25};
    enc_ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    enc_ctx->qmax = 8;
    int ret = avcodec_open2(enc_ctx, enc, NULL);
    if (ret < 0) { set_err("打开 mjpeg 编码器失败: %s", errstr(ret)); avcodec_free_context(&enc_ctx); return ERR_ENCODER; }
    scaled->pts = 0;
    ret = avcodec_send_frame(enc_ctx, scaled);
    if (ret < 0) { set_err("编码送帧失败: %s", errstr(ret)); avcodec_free_context(&enc_ctx); return ERR_ENCODER; }
    AVPacket *opkt = av_packet_alloc();
    ret = avcodec_receive_packet(enc_ctx, opkt);
    if (ret < 0) { set_err("编码取包失败: %s", errstr(ret)); av_packet_free(&opkt); avcodec_free_context(&enc_ctx); return ERR_ENCODER; }
    FILE *f = fopen(dst, "wb");
    if (!f) { set_err("无法写入输出文件"); av_packet_free(&opkt); avcodec_free_context(&enc_ctx); return ERR_OUTPUT; }
    size_t written = fwrite(opkt->data, 1, opkt->size, f);
    int result = (written == (size_t)opkt->size) ? OK : ERR_OUTPUT;
    if (result != OK) set_err("写出字节数不符（磁盘满？）");
    fclose(f);
    av_packet_free(&opkt);
    avcodec_free_context(&enc_ctx);
    return result;
}

/* 抽帧到 JPEG：open → decode → scale → write（三个入口共用） */
static int frame_to_jpeg(const char *src, const char *dst, double atSeconds, int maxSize) {
    AVFormatContext *fmt = NULL; int vidx = -1;
    int r = open_video(src, &fmt, &vidx);
    if (r != OK) return r;
    int err = ERR_DECODE;
    AVFrame *frame = decode_to_time(fmt, vidx, atSeconds, &err);
    avformat_close_input(&fmt);
    if (!frame) return err;
    AVFrame *scaled = scale_frame(frame, maxSize);
    av_frame_free(&frame);
    if (!scaled) return ERR_SCALE;
    r = write_jpeg(scaled, dst);
    av_frame_free(&scaled);
    return r;
}

/* ── JNI 入口实现 ──────────────────────────────────────────────── */

static jint jniExtractFrame(JNIEnv *env, jclass c, jstring jsrc, jstring jdst, jdouble atSec, jint maxSize) {
    (void)c;
    if (!jsrc || !jdst) return ERR_PARAM;
    const char *src = (*env)->GetStringUTFChars(env, jsrc, NULL);
    const char *dst = (*env)->GetStringUTFChars(env, jdst, NULL);
    if (!src || !dst) goto ef_out;
    g_err[0] = '\0';
    {
        jint r = frame_to_jpeg(src, dst, (double)atSec, (int)maxSize);
        (*env)->ReleaseStringUTFChars(env, jsrc, src);
        (*env)->ReleaseStringUTFChars(env, jdst, dst);
        return r;
    }
ef_out:
    if (src) (*env)->ReleaseStringUTFChars(env, jsrc, src);
    if (dst) (*env)->ReleaseStringUTFChars(env, jdst, dst);
    return ERR_PARAM;
}

/* 批量抽帧：indices 秒数组，逐点 output_dir/prefix_<i>.jpg */
static jint jniExtractThumbnails(JNIEnv *env, jclass c, jstring jsrc, jstring joutDir,
                                 jstring jprefix, jdoubleArray jtimes, jint maxSize) {
    (void)c;
    if (!jsrc || !joutDir || !jtimes) return ERR_PARAM;
    const char *src = (*env)->GetStringUTFChars(env, jsrc, NULL);
    const char *dir = (*env)->GetStringUTFChars(env, joutDir, NULL);
    const char *prefix = jprefix ? (*env)->GetStringUTFChars(env, jprefix, NULL) : "thumb";
    jint ok = 0;
    if (!src || !dir) goto et_out;
    g_err[0] = '\0';
    {
        jsize n = (*env)->GetArrayLength(env, jtimes);
        jdouble *times = (*env)->GetDoubleArrayElements(env, jtimes, NULL);
        for (jsize i = 0; i < n; i++) {
            char dst[1024];
            snprintf(dst, sizeof(dst), "%s/%s_%d.jpg", dir, prefix ? prefix : "thumb", (int)i);
            if (frame_to_jpeg(src, dst, times[i], (int)maxSize) == OK) ok++;
            else set_err("第 %d 张失败: %s", (int)i, g_err);
        }
        (*env)->ReleaseDoubleArrayElements(env, jtimes, times, JNI_ABORT);
    }
et_out:
    if (src) (*env)->ReleaseStringUTFChars(env, jsrc, src);
    if (dir) (*env)->ReleaseStringUTFChars(env, joutDir, dir);
    if (prefix) (*env)->ReleaseStringUTFChars(env, jprefix, prefix);
    return (jint)ok;
}

static jstring jniProbeVideoInfo(JNIEnv *env, jclass c, jstring jsrc) {
    (void)c;
    if (!jsrc) return NULL;
    const char *src = (*env)->GetStringUTFChars(env, jsrc, NULL);
    if (!src) return NULL;
    g_err[0] = '\0';
    AVFormatContext *fmt = NULL; int vidx = -1;
    jstring result = NULL;
    if (open_video(src, &fmt, &vidx) == OK) {
        AVStream *vs = fmt->streams[vidx];
        char codec[64] = "unknown";
        const char *cn = avcodec_get_name(vs->codecpar->codec_id);
        if (cn) snprintf(codec, sizeof(codec), "%s", cn);
        const char *pf = av_get_pix_fmt_name((enum AVPixelFormat)vs->codecpar->format);
        double dur = 0.0;
        if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) dur = fmt->duration / (double)AV_TIME_BASE;
        int fn = 0, fd = 1;
        AVRational fr = vs->avg_frame_rate;
        if (fr.num <= 0 || fr.den <= 0) fr = vs->r_frame_rate;
        if (fr.num > 0 && fr.den > 0) { fn = fr.num; fd = fr.den; }
        char *out = malloc(512);
        if (out) {
            snprintf(out, 512,
                     "{\"duration_s\":%.3f,\"width\":%d,\"height\":%d,\"codec\":\"%s\","
                     "\"pix_fmt\":\"%s\",\"bit_rate\":%lld,\"fps_num\":%d,\"fps_den\":%d}",
                     dur, vs->codecpar->width, vs->codecpar->height, codec, pf ? pf : "unknown",
                     (long long)vs->codecpar->bit_rate, fn, fd);
            result = (*env)->NewStringUTF(env, out);
            free(out);
        }
        avformat_close_input(&fmt);
    }
    (*env)->ReleaseStringUTFChars(env, jsrc, src);
    return result;
}

static jstring jniProbeAudioInfo(JNIEnv *env, jclass c, jstring jsrc) {
    (void)c;
    if (!jsrc) return NULL;
    const char *src = (*env)->GetStringUTFChars(env, jsrc, NULL);
    if (!src) return NULL;
    g_err[0] = '\0';
    AVFormatContext *fmt = NULL;
    jstring result = NULL;
    if (open_input(src, &fmt) == OK) {
        int aidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
        if (aidx >= 0) {
            AVStream *as = fmt->streams[aidx];
            char codec[64] = "unknown";
            const char *cn = avcodec_get_name(as->codecpar->codec_id);
            if (cn) snprintf(codec, sizeof(codec), "%s", cn);
            double dur = 0.0;
            if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) dur = fmt->duration / (double)AV_TIME_BASE;
            char *out = malloc(384);
            if (out) {
                snprintf(out, 384,
                         "{\"duration_s\":%.3f,\"codec\":\"%s\",\"sample_rate\":%d,"
                         "\"channels\":%d,\"bit_rate\":%lld}",
                         dur, codec, as->codecpar->sample_rate,
                         as->codecpar->ch_layout.nb_channels, (long long)as->codecpar->bit_rate);
                result = (*env)->NewStringUTF(env, out);
                free(out);
            }
        } else {
            set_err("没有音频流");
        }
        avformat_close_input(&fmt);
    }
    (*env)->ReleaseStringUTFChars(env, jsrc, src);
    return result;
}

static jdouble jniProbeDuration(JNIEnv *env, jclass c, jstring jsrc) {
    (void)c;
    if (!jsrc) return 0.0;
    const char *src = (*env)->GetStringUTFChars(env, jsrc, NULL);
    if (!src) return 0.0;
    AVFormatContext *fmt = NULL;
    double d = 0.0;
    if (open_input(src, &fmt) == OK) {
        if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0)
            d = fmt->duration / (double)AV_TIME_BASE;
        avformat_close_input(&fmt);
    }
    (*env)->ReleaseStringUTFChars(env, jsrc, src);
    return (jdouble)d;
}

/*
 * 通用 stream copy：把 [startSec, endSec) 区间的包原样拷到新容器。
 * clipVideo 与 remuxVideo 共用（remux 传 endSec = 无穷）。
 * 关键点：输入包时间戳必须先平移到 0 起点，否则多数 muxer 会写负时间戳或错位。
 */
static int stream_copy(const char *src, const char *dst, double startSec, double endSec) {
    AVFormatContext *in = NULL, *out = NULL;
    int *map = NULL;
    int r = ERR_MUX;
    if (avformat_open_input(&in, src, NULL, NULL) < 0) { set_err("打开输入失败"); return ERR_OPEN; }
    if (avformat_find_stream_info(in, NULL) < 0) { set_err("读流信息失败"); goto done; }
    if (avformat_alloc_output_context2(&out, NULL, NULL, dst) < 0) { set_err("无法按扩展名推断输出容器"); goto done; }

    map = av_calloc(in->nb_streams, sizeof(int));
    if (!map) { set_err("内存不足"); goto done; }
    for (unsigned i = 0; i < in->nb_streams; i++) {
        map[i] = -1;
        /* 只拷贝音视频（数据/字幕流跨容器常不兼容） */
        if (in->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            in->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        AVStream *os = avformat_new_stream(out, NULL);
        if (!os) { set_err("建输出流失败"); goto done; }
        if (avcodec_parameters_copy(os->codecpar, in->streams[i]->codecpar) < 0) { set_err("参数拷贝失败"); goto done; }
        os->codecpar->codec_tag = 0; /* 换容器时清掉原 tag，避免 muxer 拒绝 */
        os->time_base = in->streams[i]->time_base;
        map[i] = os->index;
    }
    if (!(out->oformat->flags & AVFMT_NOFILE) && avio_open(&out->pb, dst, AVIO_FLAG_WRITE) < 0) {
        set_err("无法创建输出文件"); goto done;
    }
    if (avformat_write_header(out, NULL) < 0) { set_err("写头失败"); goto done; }

    if (startSec > 0) {
        if (av_seek_frame(in, -1, (int64_t)(startSec * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD) < 0)
            set_err("seek 失败（从头开始）");
    }
    {
        AVPacket *pkt = av_packet_alloc();
        int64_t *first_dts = av_calloc(in->nb_streams, sizeof(int64_t));
        for (unsigned i = 0; i < in->nb_streams; i++) first_dts[i] = AV_NOPTS_VALUE;
        int budget = 500000; /* 包数上限，防异常文件跑飞 */
        while (budget-- > 0 && av_read_frame(in, pkt) >= 0) {
            int si = pkt->stream_index;
            if (si < 0 || (unsigned)si >= in->nb_streams || map[si] < 0) { av_packet_unref(pkt); continue; }
            AVStream *is = in->streams[si], *os = out->streams[map[si]];
            double pts_s = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts * av_q2d(is->time_base) : -1.0;
            if (pts_s >= 0 && pts_s > endSec) { av_packet_unref(pkt); break; }
            if (first_dts[si] == AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE) first_dts[si] = pkt->dts;
            if (first_dts[si] != AV_NOPTS_VALUE) {
                if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= first_dts[si];
                if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= first_dts[si];
            }
            av_packet_rescale_ts(pkt, is->time_base, os->time_base);
            pkt->stream_index = map[si];
            av_interleaved_write_frame(out, pkt);
            av_packet_unref(pkt);
        }
        av_free(first_dts);
        av_packet_free(&pkt);
    }
    av_write_trailer(out);
    r = OK;
done:
    av_free(map);
    if (out) {
        if (!(out->oformat->flags & AVFMT_NOFILE) && out->pb) avio_closep(&out->pb);
        avformat_free_context(out);
    }
    if (in) avformat_close_input(&in);
    return r;
}

static jint jniClipVideo(JNIEnv *env, jclass c, jstring jsrc, jstring jdst, jdouble startSec, jdouble endSec) {
    (void)c;
    if (!jsrc || !jdst || endSec <= startSec) return ERR_PARAM;
    const char *src = (*env)->GetStringUTFChars(env, jsrc, NULL);
    const char *dst = (*env)->GetStringUTFChars(env, jdst, NULL);
    jint r = ERR_PARAM;
    if (src && dst) { g_err[0] = '\0'; r = stream_copy(src, dst, startSec, endSec); }
    if (src) (*env)->ReleaseStringUTFChars(env, jsrc, src);
    if (dst) (*env)->ReleaseStringUTFChars(env, jdst, dst);
    return r;
}

static jint jniRemuxVideo(JNIEnv *env, jclass c, jstring jsrc, jstring jdst) {
    return jniClipVideo(env, c, jsrc, jdst, 0.0, 1e12);
}

/*
 * 音轨提取/重编码。
 * codec="copy"：原样打包（无损、快）；"flac"/"aac"：重编码（需 common 档启用编码器）。
 * 重编码经 swresample 做采样格式/率/声道统一，末帧不足按静音补齐（编码器要求定长帧）。
 */
static jint jniExtractAudio(JNIEnv *env, jclass c, jstring jsrc, jstring jdst, jstring jcodec) {
    (void)c;
    if (!jsrc || !jdst || !jcodec) return ERR_PARAM;
    const char *src = (*env)->GetStringUTFChars(env, jsrc, NULL);
    const char *dst = (*env)->GetStringUTFChars(env, jdst, NULL);
    const char *codec = (*env)->GetStringUTFChars(env, jcodec, NULL);
    if (!src || !dst || !codec) goto ea_out;
    g_err[0] = '\0';
    {
        int copy_mode = (strcmp(codec, "copy") == 0);
        AVFormatContext *in = NULL, *out = NULL;
        AVCodecContext *dec_ctx = NULL, *enc_ctx = NULL;
        SwrContext *swr = NULL;
        AVStream *ast = NULL, *ost = NULL;
        int aidx = -1;
        jint r = ERR_MUX;

        if (copy_mode) {
            r = stream_copy(src, dst, 0.0, 1e12);
            goto ea_release;
        }

        if (avformat_open_input(&in, src, NULL, NULL) < 0) { set_err("打开输入失败"); r = ERR_OPEN; goto ea_release; }
        if (avformat_find_stream_info(in, NULL) < 0) { set_err("读流信息失败"); goto ea_release; }
        aidx = av_find_best_stream(in, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
        if (aidx < 0) { set_err("没有音频流"); r = ERR_STREAM; goto ea_release; }
        ast = in->streams[aidx];

        {
            const AVCodec *dec = avcodec_find_decoder(ast->codecpar->codec_id);
            if (!dec) { set_err("未编译该音频解码器"); r = ERR_DECODER; goto ea_release; }
            dec_ctx = avcodec_alloc_context3(dec);
            avcodec_parameters_to_context(dec_ctx, ast->codecpar);
            if (avcodec_open2(dec_ctx, dec, NULL) < 0) { set_err("打开音频解码器失败"); r = ERR_DECODER; goto ea_release; }
        }
        {
            const AVCodec *enc = avcodec_find_encoder_by_name(codec);
            if (!enc) { set_err("编码器 '%s' 未编译进本档位（可选 copy|flac|aac）", codec); r = ERR_UNSUPPORTED; goto ea_release; }
            enc_ctx = avcodec_alloc_context3(enc);
            enc_ctx->sample_rate = dec_ctx->sample_rate ? dec_ctx->sample_rate : 44100;
            if (av_channel_layout_check(&dec_ctx->ch_layout)) av_channel_layout_copy(&enc_ctx->ch_layout, &dec_ctx->ch_layout);
            else av_channel_layout_default(&enc_ctx->ch_layout, 2);
            enc_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
            /* FFmpeg 8.0：AVCodec.sample_fmts 移除，改用 avcodec_get_supported_config */
            {
                const enum AVSampleFormat *fmts = NULL;
                int nfmts = 0;
                if (avcodec_get_supported_config(NULL, enc, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                                 0, (const void **)&fmts, &nfmts) == 0 && nfmts > 0 && fmts)
                    enc_ctx->sample_fmt = fmts[0];
            }
#else
            /* FFmpeg <=7.x 旧 API */
            if (enc->sample_fmts) enc_ctx->sample_fmt = enc->sample_fmts[0];
#endif
            enc_ctx->bit_rate = 128000;
            if (avformat_alloc_output_context2(&out, NULL, NULL, dst) < 0) { set_err("无法推断输出容器（请用 .m4a/.aac/.flac 后缀）"); r = ERR_MUX; goto ea_release; }
            if (out->oformat->flags & AVFMT_GLOBALHEADER) enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (avcodec_open2(enc_ctx, enc, NULL) < 0) { set_err("打开音频编码器失败"); r = ERR_ENCODER; goto ea_release; }
            ost = avformat_new_stream(out, NULL);
            if (!ost) { set_err("建输出流失败"); goto ea_release; }
            if (avcodec_parameters_from_context(ost->codecpar, enc_ctx) < 0) { set_err("参数写出失败"); goto ea_release; }
            ost->time_base = enc_ctx->time_base;
            if (!(out->oformat->flags & AVFMT_NOFILE) && avio_open(&out->pb, dst, AVIO_FLAG_WRITE) < 0) { set_err("无法创建输出文件"); goto ea_release; }
            if (avformat_write_header(out, NULL) < 0) { set_err("写头失败"); goto ea_release; }

            swr = swr_alloc();
            av_opt_set_chlayout(swr, "in_chlayout", &dec_ctx->ch_layout, 0);
            av_opt_set_chlayout(swr, "out_chlayout", &enc_ctx->ch_layout, 0);
            av_opt_set_int(swr, "in_sample_rate", dec_ctx->sample_rate, 0);
            av_opt_set_int(swr, "out_sample_rate", enc_ctx->sample_rate, 0);
            av_opt_set_sample_fmt(swr, "in_sample_fmt", dec_ctx->sample_fmt, 0);
            av_opt_set_sample_fmt(swr, "out_sample_fmt", enc_ctx->sample_fmt, 0);
            if (swr_init(swr) < 0) { set_err("swresample 初始化失败"); r = ERR_ENCODER; goto ea_release; }
        }

        {
            int out_ns = enc_ctx->frame_size > 0 ? enc_ctx->frame_size : 1024;
            AVFrame *of = av_frame_alloc();
            of->format = enc_ctx->sample_fmt;
            av_channel_layout_copy(&of->ch_layout, &enc_ctx->ch_layout);
            of->sample_rate = enc_ctx->sample_rate;
            of->nb_samples = out_ns;
            if (av_frame_get_buffer(of, 0) < 0) { set_err("分配编码帧失败"); av_frame_free(&of); goto ea_release; }

            AVFrame *frame = av_frame_alloc();
            AVPacket *pkt = av_packet_alloc(), *opkt = av_packet_alloc();
            int64_t pts = 0;
            int budget = 500000;
            while (budget-- > 0 && av_read_frame(in, pkt) >= 0) {
                if (pkt->stream_index != aidx) { av_packet_unref(pkt); continue; }
                if (avcodec_send_packet(dec_ctx, pkt) < 0) { av_packet_unref(pkt); continue; }
                av_packet_unref(pkt);
                while (avcodec_receive_frame(dec_ctx, frame) == 0) {
                    int n = swr_convert(swr, of->data, out_ns, (const uint8_t **)frame->data, frame->nb_samples);
                    av_frame_unref(frame);
                    if (n <= 0) continue;
                    of->nb_samples = n;
                    of->pts = pts;
                    pts += n;
                    if (avcodec_send_frame(enc_ctx, of) < 0) continue;
                    while (avcodec_receive_packet(enc_ctx, opkt) == 0) {
                        av_packet_rescale_ts(opkt, enc_ctx->time_base, ost->time_base);
                        opkt->stream_index = ost->index;
                        av_interleaved_write_frame(out, opkt);
                        av_packet_unref(opkt);
                    }
                }
            }
            /* flush 解码器 */
            avcodec_send_packet(dec_ctx, NULL);
            while (avcodec_receive_frame(dec_ctx, frame) == 0) {
                int n = swr_convert(swr, of->data, out_ns, (const uint8_t **)frame->data, frame->nb_samples);
                av_frame_unref(frame);
                if (n <= 0) continue;
                of->nb_samples = n; of->pts = pts; pts += n;
                if (avcodec_send_frame(enc_ctx, of) < 0) continue;
                while (avcodec_receive_packet(enc_ctx, opkt) == 0) {
                    av_packet_rescale_ts(opkt, enc_ctx->time_base, ost->time_base);
                    opkt->stream_index = ost->index;
                    av_interleaved_write_frame(out, opkt); av_packet_unref(opkt);
                }
            }
            /* flush swr 残余（末帧不足补静音，编码器要定长帧） */
            for (;;) {
                int n = swr_convert(swr, of->data, out_ns, NULL, 0);
                if (n <= 0) break;
                if (n < out_ns) {
                    int bytes = av_get_bytes_per_sample(enc_ctx->sample_fmt);
                    for (int ch = 0; ch < enc_ctx->ch_layout.nb_channels; ch++) {
                        if (of->data[ch]) memset(of->data[ch] + (size_t)n * bytes, 0, (size_t)(out_ns - n) * bytes);
                    }
                    n = out_ns;
                }
                of->nb_samples = n; of->pts = pts; pts += n;
                if (avcodec_send_frame(enc_ctx, of) < 0) break;
                while (avcodec_receive_packet(enc_ctx, opkt) == 0) {
                    av_packet_rescale_ts(opkt, enc_ctx->time_base, ost->time_base);
                    opkt->stream_index = ost->index;
                    av_interleaved_write_frame(out, opkt); av_packet_unref(opkt);
                }
            }
            avcodec_send_frame(enc_ctx, NULL);
            while (avcodec_receive_packet(enc_ctx, opkt) == 0) {
                av_packet_rescale_ts(opkt, enc_ctx->time_base, ost->time_base);
                opkt->stream_index = ost->index;
                av_interleaved_write_frame(out, opkt); av_packet_unref(opkt);
            }
            av_frame_free(&of);
            av_frame_free(&frame);
            av_packet_free(&pkt);
            av_packet_free(&opkt);
            av_write_trailer(out);
            r = OK;
        }

ea_release:
        if (swr) swr_free(&swr);
        if (dec_ctx) avcodec_free_context(&dec_ctx);
        if (enc_ctx) avcodec_free_context(&enc_ctx);
        if (out) {
            if (!(out->oformat->flags & AVFMT_NOFILE) && out->pb) avio_closep(&out->pb);
            avformat_free_context(out);
        }
        if (in) avformat_close_input(&in);
        (*env)->ReleaseStringUTFChars(env, jsrc, src);
        (*env)->ReleaseStringUTFChars(env, jdst, dst);
        (*env)->ReleaseStringUTFChars(env, jcodec, codec);
        return r;
    }
ea_out:
    if (src) (*env)->ReleaseStringUTFChars(env, jsrc, src);
    if (dst) (*env)->ReleaseStringUTFChars(env, jdst, dst);
    if (codec) (*env)->ReleaseStringUTFChars(env, jcodec, codec);
    return ERR_PARAM;
}

/*
 * 单帧滤镜出图：解码到 atSeconds → filterGraph → JPEG。
 * graph 例："scale=640:-1,transpose=1"。可用滤镜受档位限制（见 BUILD.md）。
 */
static jint jniApplyFilter(JNIEnv *env, jclass c, jstring jsrc, jstring jdst, jstring jgraph,
                           jdouble atSec, jint maxSize) {
    (void)c;
    if (!jsrc || !jdst || !jgraph) return ERR_PARAM;
    const char *src = (*env)->GetStringUTFChars(env, jsrc, NULL);
    const char *dst = (*env)->GetStringUTFChars(env, jdst, NULL);
    const char *graph = (*env)->GetStringUTFChars(env, jgraph, NULL);
    jint r = ERR_PARAM;
    AVFormatContext *fmt = NULL;
    int vidx = -1;
    AVFrame *frame = NULL;
    AVFilterGraph *fg = NULL;
    if (!src || !dst || !graph) goto af_out;
    g_err[0] = '\0';
    if (open_video(src, &fmt, &vidx) != OK) { r = ERR_OPEN; goto af_out; }
    {
        int err = ERR_DECODE;
        frame = decode_to_time(fmt, vidx, (double)atSec, &err);
        if (!frame) { r = err; avformat_close_input(&fmt); goto af_out; }
        /* 滤镜链参数取自解码前流信息，故先取出再关 fmt */
        {
            AVStream *vs = fmt->streams[vidx];
            AVRational fr = vs->avg_frame_rate;
            if (fr.num <= 0 || fr.den <= 0) fr = vs->r_frame_rate;
            AVRational sar = vs->sample_aspect_ratio;
            if (sar.num <= 0 || sar.den <= 0) sar = (AVRational){1, 1};
            avformat_close_input(&fmt);

            const AVFilter *buffersrc = avfilter_get_by_name("buffer");
            const AVFilter *buffersink = avfilter_get_by_name("buffersink");
            if (!buffersrc || !buffersink) { set_err("avfilter 未编译（buffer/buffersink 缺失）"); r = ERR_UNSUPPORTED; goto af_out; }
            fg = avfilter_graph_alloc();
            if (!fg) { set_err("分配 filtergraph 失败"); r = ERR_FILTER; goto af_out; }

            char args[512];
            snprintf(args, sizeof(args),
                     "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:sar=%d/%d",
                     frame->width, frame->height, frame->format,
                     fr.den, fr.num, sar.num, sar.den, sar.num, sar.den);

            AVFilterContext *src_ctx = NULL, *sink_ctx = NULL;
            if (avfilter_graph_create_filter(&src_ctx, buffersrc, "in", args, NULL, fg) < 0 ||
                avfilter_graph_create_filter(&sink_ctx, buffersink, "out", NULL, NULL, fg) < 0) {
                set_err("创建 buffer/buffersink 失败");
                r = ERR_FILTER; goto af_out;
            }
            AVFilterInOut *ins = avfilter_inout_alloc();
            AVFilterInOut *outs = avfilter_inout_alloc();
            if (!ins || !outs) { set_err("分配 in/out 失败"); r = ERR_FILTER; goto af_out; }
            outs->name = av_strdup("in");
            outs->filter_ctx = src_ctx;
            outs->pad_idx = 0;
            outs->next = NULL;
            ins->name = av_strdup("out");
            ins->filter_ctx = sink_ctx;
            ins->pad_idx = 0;
            ins->next = NULL;
            int pr = avfilter_graph_parse_ptr(fg, graph, &ins, &outs, NULL);
            avfilter_inout_free(&ins);
            avfilter_inout_free(&outs);
            if (pr < 0 || avfilter_graph_config(fg, NULL) < 0) {
                set_err("滤镜链配置失败（本档位可能未编译该滤镜）: %s", graph);
                r = ERR_FILTER; goto af_out;
            }
            if (av_buffersrc_add_frame_flags(src_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                set_err("滤镜送帧失败"); r = ERR_FILTER; goto af_out;
            }
            AVFrame *filtered = av_frame_alloc();
            if (av_buffersink_get_frame(sink_ctx, filtered) >= 0) {
                AVFrame *scaled = scale_frame(filtered, (int)maxSize);
                if (scaled) { r = write_jpeg(scaled, dst); av_frame_free(&scaled); }
                else r = ERR_SCALE;
            } else {
                set_err("滤镜链没有产出帧"); r = ERR_FILTER;
            }
            av_frame_free(&filtered);
        }
    }
af_out:
    if (fg) avfilter_graph_free(&fg);
    if (frame) av_frame_free(&frame);
    if (fmt) avformat_close_input(&fmt);
    if (src) (*env)->ReleaseStringUTFChars(env, jsrc, src);
    if (dst) (*env)->ReleaseStringUTFChars(env, jdst, dst);
    if (graph) (*env)->ReleaseStringUTFChars(env, jgraph, graph);
    return r;
}

static jstring jniLastError(JNIEnv *env, jclass c) {
    (void)c;
    return (*env)->NewStringUTF(env, g_err);
}

static jstring jniBridgeVersion(JNIEnv *env, jclass c) {
    (void)c;
    char buf[192];
    snprintf(buf, sizeof(buf), "ffmpeg-bridge/1.0 (libav %s)", av_version_info());
    return (*env)->NewStringUTF(env, buf);
}

/* ── RegisterNatives：包名零耦合的关键 ─────────────────────────── */
static const JNINativeMethod g_methods[] = {
    { "extractFrame",         "(Ljava/lang/String;Ljava/lang/String;DI)I",                    (void *)jniExtractFrame },
    { "extractThumbnails",    "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[DI)I",  (void *)jniExtractThumbnails },
    { "probeVideoInfo",       "(Ljava/lang/String;)Ljava/lang/String;",                        (void *)jniProbeVideoInfo },
    { "probeAudioInfo",       "(Ljava/lang/String;)Ljava/lang/String;",                        (void *)jniProbeAudioInfo },
    { "probeDurationSeconds", "(Ljava/lang/String;)D",                                         (void *)jniProbeDuration },
    { "clipVideo",            "(Ljava/lang/String;Ljava/lang/String;DD)I",                     (void *)jniClipVideo },
    { "remuxVideo",           "(Ljava/lang/String;Ljava/lang/String;)I",                       (void *)jniRemuxVideo },
    { "extractAudio",         "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",     (void *)jniExtractAudio },
    { "applyFilter",          "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;DI)I",   (void *)jniApplyFilter },
    { "lastError",            "()Ljava/lang/String;",                                          (void *)jniLastError },
    { "bridgeVersion",        "()Ljava/lang/String;",                                          (void *)jniBridgeVersion },
};

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass clazz = (*env)->FindClass(env, BRIDGE_CLASS);
    if (!clazz) {
        /* Java 还没法调进来，g_err 无处可显，直接 stderr（logcat 可见） */
        fprintf(stderr, "[ffmpeg_bridge] FindClass(%s) 失败 —— bridge_config.txt 与 Java 类包名未同步\n", BRIDGE_CLASS);
        return JNI_ERR;
    }
    if ((*env)->RegisterNatives(env, clazz, g_methods, sizeof(g_methods) / sizeof(g_methods[0])) != JNI_OK) {
        fprintf(stderr, "[ffmpeg_bridge] RegisterNatives 失败\n");
        return JNI_ERR;
    }
    av_log_set_level(AV_LOG_ERROR);
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *reserved) {
    (void)vm; (void)reserved;
}
