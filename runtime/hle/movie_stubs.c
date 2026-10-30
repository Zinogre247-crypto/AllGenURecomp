// [AllGenU-Android] Stubs de FFmpeg para el primer build de Android.
//
// runtime/hle/movie.c (cinematicas) usa libavformat/libavcodec. Compilar FFmpeg
// para arm64 en CI son ~30-60 min mas; para poder obtener un APK antes, este
// archivo da las MISMAS funciones devolviendo error: el reproductor de video
// falla limpiamente (movie:: devuelve MOVIE_ERR) y el juego sigue, pero las
// cinematicas no se ven.
//
// Para quitarlo y tener cinematicas de verdad:
//   1. compila FFmpeg para Android (o usa uno prebuilt),
//   2. en runtime/CMakeLists.txt pon BUILD_FFMPEG_STUBS=OFF y enlaza
//      -lavformat -lavcodec -lavutil,
//   3. borra el archivo del target.
#include <errno.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>

#define FALLA AVERROR(ENOSYS)

int avformat_open_input(AVFormatContext **ps, const char *url, const AVInputFormat *fmt,
                        AVDictionary **options) {
    (void)ps; (void)url; (void)fmt; (void)options;
    return FALLA;
}

int avformat_find_stream_info(AVFormatContext *ic, AVDictionary **options) {
    (void)ic; (void)options;
    return FALLA;
}

void avformat_close_input(AVFormatContext **s) { (void)s; }

int av_read_frame(AVFormatContext *s, AVPacket *pkt) { (void)s; (void)pkt; return FALLA; }

int av_seek_frame(AVFormatContext *s, int stream_index, int64_t timestamp, int flags) {
    (void)s; (void)stream_index; (void)timestamp; (void)flags;
    return FALLA;
}

const AVCodec *avcodec_find_decoder(enum AVCodecID id) { (void)id; return NULL; }

AVCodecContext *avcodec_alloc_context3(const AVCodec *codec) { (void)codec; return NULL; }

void avcodec_free_context(AVCodecContext **avctx) { (void)avctx; }

int avcodec_parameters_to_context(AVCodecContext *codec, const AVCodecParameters *par) {
    (void)codec; (void)par;
    return FALLA;
}

int avcodec_send_packet(AVCodecContext *avctx, const AVPacket *avpkt) {
    (void)avctx; (void)avpkt;
    return FALLA;
}

int avcodec_receive_frame(AVCodecContext *avctx, AVFrame *frame) {
    (void)avctx; (void)frame;
    return FALLA;
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
