#include <stdio.h>
#include <libavformat/avformat.h>

static AVStream *add_output_stream(AVFormatContext * output_format_context, AVStream * input_stream) {
    AVCodecContext *input_codec_context = NULL;
    AVCodecContext *output_codec_context = NULL;

    AVStream *output_stream = NULL;
    output_stream = avformat_new_stream(output_format_context, 0);
    if (!output_stream) {
        printf("Call avformat_new_stream function failed\n");
        return NULL;
    }

    input_codec_context = input_stream->codec;
    output_codec_context = output_stream->codec;

    output_codec_context->codec_id = input_codec_context->codec_id;
    output_codec_context->codec_type = input_codec_context->codec_type;
    output_codec_context->codec_tag = input_codec_context->codec_tag;
    output_codec_context->bit_rate = input_codec_context->bit_rate;
    output_codec_context->extradata = input_codec_context->extradata;
    output_codec_context->extradata_size = input_codec_context->extradata_size;

    if (av_q2d(input_codec_context->time_base) * input_codec_context->ticks_per_frame > av_q2d(input_stream->time_base) && av_q2d(input_stream->time_base) < 1.0 / 1000) {
        output_codec_context->time_base = input_codec_context->time_base;
        output_codec_context->time_base.num *= input_codec_context->ticks_per_frame;
    } else {
        output_codec_context->time_base = input_stream->time_base;
    }
    switch (input_codec_context->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            output_codec_context->channel_layout = input_codec_context->channel_layout;
            output_codec_context->sample_rate = input_codec_context->sample_rate;
            output_codec_context->channels = input_codec_context->channels;
            output_codec_context->frame_size = input_codec_context->frame_size;
            if ((input_codec_context->block_align == 1 && input_codec_context->codec_id == CODEC_ID_MP3) || input_codec_context->codec_id == CODEC_ID_AC3) {
                output_codec_context->block_align = 0;
            } else {
                output_codec_context->block_align = input_codec_context->block_align;
            }
            break;
        case AVMEDIA_TYPE_VIDEO:
            output_codec_context->pix_fmt = input_codec_context->pix_fmt;
            output_codec_context->width = input_codec_context->width;
            output_codec_context->height = input_codec_context->height;
            output_codec_context->has_b_frames = input_codec_context->has_b_frames;
            if (output_format_context->oformat->flags & AVFMT_GLOBALHEADER) {
                output_codec_context->flags |= CODEC_FLAG_GLOBAL_HEADER;
            }
            break;
        default:
            break;
    }

    return output_stream;
}

int main(int argc, char *argv[]) {

    const char *input = "ts.h264";
    const char *output_prefix = NULL;
    char *segment_duration_check = 0;
    const char *index = NULL;
    char *tmp_index = NULL;
    const char *http_prefix = NULL;
    long max_tsfiles = 0;
    double prev_segment_time = 0;
    double segment_duration = 0;

    AVInputFormat *ifmt = NULL;
    AVOutputFormat *ofmt = NULL;
    AVFormatContext *ic = NULL;
    AVFormatContext *oc = NULL;
    AVStream *video_st = NULL;
    AVStream *audio_st = NULL;
    AVCodec *codec = NULL;
    AVDictionary *pAVDictionary = NULL;

    av_register_all();

    char szError[256] = {0};
    int nRet = avformat_open_input(&ic, input, ifmt, &pAVDictionary);
    if (nRet != 0) {
        av_strerror(nRet, szError, 256);
        printf("%s\n", szError);
        printf("Call avformat_open_input function failed!\n");
        return 0;
    }

    if (avformat_find_stream_info(ic, 0) < 0) {
        printf("Call avformat_find_stream_info function failed!\n");
        return 0;
    }

    ofmt = av_guess_format("mpegts", NULL, NULL);
    if (!ofmt) {
        printf("Call av_guess_format function failed!\n");
        return 0;
    }

    oc = avformat_alloc_context();
    if (!oc) {
        printf("Call av_guess_format function failed!\n");
        return 0;
    }
    oc->oformat = ofmt;

    int video_index = -1, audio_index = -1;
    for (unsigned int i = 0; i < ic->nb_streams && (video_index < 0 || audio_index < 0); i++) {
        switch (ic->streams[i]->codec->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                video_index = i;
                ic->streams[i]->discard = AVDISCARD_NONE;
                video_st = add_output_stream(oc, ic->streams[i]);
                break;
            case AVMEDIA_TYPE_AUDIO:
                audio_index = i;
                ic->streams[i]->discard = AVDISCARD_NONE;
                audio_st = add_output_stream(oc, ic->streams[i]);
                break;
            default:
                ic->streams[i]->discard = AVDISCARD_ALL;
                break;
        }
    }
    codec = avcodec_find_decoder(video_st->codec->codec_id);
    if (codec == NULL) {
        printf("Call avcodec_find_decoder function failed!\n");
        return 0;
    }

    if (avcodec_open2(video_st->codec, codec, 0) < 0) {
        printf("Call avcodec_open2 function failed !\n");
        return 0;
    }

    if (avio_open(&oc->pb, "264.ts", AVIO_FLAG_WRITE) < 0) {
        return 0;
    }

    if (avformat_write_header(oc, &pAVDictionary)) {
        printf("Call avformat_write_header function failed.\n");
        return 0;
    }

    uint8_t *dummy = NULL;
    int dummy_size = 0;
    AVBitStreamFilterContext *bsfc = av_bitstream_filter_init("h264_mp4toannexb");
    if (bsfc == NULL) {
        return -1;
    }

    int decode_done = 0;
    do {
        double segment_time = 0;
        AVPacket packet;
        decode_done = av_read_frame(ic, &packet);
        if (decode_done < 0)
            break;

        if (av_dup_packet(&packet) < 0) {
            printf("Call av_dup_packet function failed\n");
            av_free_packet(&packet);
            break;
        }

        static int nCount = 0;
        if (nCount++ < 20) {
            printf("The packet.stream_index is %d\n", packet.stream_index);
        }

        if (packet.stream_index == video_index && (packet.flags & AV_PKT_FLAG_KEY)) {
            segment_time = (double) video_st->pts.val * video_st->time_base.num / video_st->time_base.den;
        }
        else if (packet.stream_index == audio_index) {
            segment_time = (double) audio_st->pts.val * audio_st->time_base.num / audio_st->time_base.den;
        }
        else {
            segment_time = prev_segment_time;
        }
        nRet = av_interleaved_write_frame(oc, &packet);
        if (nRet < 0) {
            printf("Call av_interleaved_write_frame function failed\n");
        }
        else if (nRet > 0) {
            printf("End of stream requested\n");
            av_free_packet(&packet);
            break;
        }
        av_free_packet(&packet);
    } while (!decode_done);

    av_write_trailer(oc);

    av_bitstream_filter_close(bsfc);
    avcodec_close(video_st->codec);
    for (unsigned int k = 0; k < oc->nb_streams; k++) {
        av_freep(&oc->streams[k]->codec);
        av_freep(&oc->streams[k]);
    }
    av_free(oc);
    getchar();
    return 0;
}