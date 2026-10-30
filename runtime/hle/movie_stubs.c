// [AllGenU-Android] Stubs de FFmpeg para el build de Android.
//
// runtime/hle/movie.c (cinematicas) usa libavformat/libavcodec. Compilar FFmpeg
// para arm64 son 30-60 min extra en CI, asi que estas funciones devuelven error:
// movie:: falla limpiamente y el juego sigue, pero las cinematicas no se ven.
//
// OJO: aqui solo van las funciones que movie.c USA de verdad (se comprobo con
// los headers reales de FFmpeg 7.1). Si movie.c cambia, hay que anadir las suyas.
//
// Para tener video real:
//   1) compila FFmpeg para arm64 (o usa un prebuilt),
//   2) -DBUILD_FFMPEG_STUBS=OFF -DFFMPEG_INCLUDE_DIR=... -DFFMPEG_LIB_DIR=...
//   3) este archivo deja de compilarse.
#include <errno.h>
#include <stddef.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>

#define STUB_RET AVERROR(ENOSYS)

int avformat_open_input(AVFormatContext **ps, const char *url, const AVInputFormat *fmt,
                        AVDictionary **options) {
    (void)ps; (void)url; (void)fmt; (void)options;
    return STUB_RET;
}

int avformat_find_stream_info(AVFormatContext *ic, AVDictionary **options) {
    (void)ic; (void)options;
    return STUB_RET;
}

void avformat_close_input(AVFormatContext **s) { (void)s; }

int av_read_frame(AVFormatContext *s, AVPacket *pkt) { (void)s; (void)pkt; return STUB_RET; }

int av_seek_frame(AVFormatContext *s, int stream_index, int64_t timestamp, int flags) {
    (void)s; (void)stream_index; (void)timestamp; (void)flags;
    return STUB_RET;
}

const AVCodec *avcodec_find_decoder(enum AVCodecID id) { (void)id; return NULL; }

AVCodecContext *avcodec_alloc_context3(const AVCodec *codec) { (void)codec; return NULL; }

int avcodec_open2(AVCodecContext *avctx, const AVCodec *codec, AVDictionary **options) {
    (void)avctx; (void)codec; (void)options;
    return STUB_RET;
}

void avcodec_free_context(AVCodecContext **avctx) { (void)avctx; }

int avcodec_parameters_to_context(AVCodecContext *codec, const AVCodecParameters *par) {
    (void)codec; (void)par;
    return STUB_RET;
}

int avcodec_send_packet(AVCodecContext *avctx, const AVPacket *avpkt) {
    (void)avctx; (void)avpkt;
    return STUB_RET;
}

int avcodec_receive_frame(AVCodecContext *avctx, AVFrame *frame) {
    (void)avctx; (void)frame;
    return STUB_RET;
}

void avcodec_flush_buffers(AVCodecContext *avctx) { (void)avctx; }

AVPacket *av_packet_alloc(void) { return NULL; }
void av_packet_free(AVPacket **pkt) { (void)pkt; }
void av_packet_unref(AVPacket *pkt) { (void)pkt; }

AVFrame *av_frame_alloc(void) { return NULL; }
void av_frame_free(AVFrame **frame) { (void)frame; }
AVFrame *av_frame_clone(const AVFrame *src) { (void)src; return NULL; }

int64_t av_rescale_q(int64_t a, AVRational bq, AVRational cq) {
    (void)bq; (void)cq;
    return a;
}
