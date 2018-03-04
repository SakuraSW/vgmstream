#include <math.h>
#include "coding.h"
#include "vorbis_custom_decoder.h"

#ifdef VGM_USE_VORBIS
#include <vorbis/codec.h>

#define VORBIS_DEFAULT_BUFFER_SIZE 0x8000 /* should be at least the size of the setup header, ~0x2000 */

static void pcm_convert_float_to_16(vorbis_custom_codec_data * data, sample * outbuf, int samples_to_do, float ** pcm);

/**
 * Inits a vorbis stream of some custom variety.
 *
 * Normally Vorbis packets are stored in .ogg, which is divided into OggS pages/packets, and the first packets contain necessary
 * Vorbis setup. For custom vorbis the OggS layer is replaced/optimized, the setup can be modified or stored elsewhere
 * (i.e.- in the .exe) and raw Vorbis packets may be modified as well, presumably to shave off some kb and/or obfuscate.
 * We'll manually read/modify the data and decode it with libvorbis calls.
 *
 * Reference: https://www.xiph.org/vorbis/doc/libvorbis/overview.html
 */
vorbis_custom_codec_data * init_vorbis_custom(STREAMFILE *streamFile, off_t start_offset, vorbis_custom_t type, vorbis_custom_config * config) {
    vorbis_custom_codec_data * data = NULL;
    int ok;

    /* init stuff */
    data = calloc(1,sizeof(vorbis_custom_codec_data));
    if (!data) goto fail;

    data->buffer_size = VORBIS_DEFAULT_BUFFER_SIZE;
    data->buffer = calloc(sizeof(uint8_t), data->buffer_size);
    if (!data->buffer) goto fail;

    /* keep around to decode too */
    data->type = type;
    memcpy(&data->config, config, sizeof(vorbis_custom_config));


    /* init vorbis stream state, using 3 fake Ogg setup packets (info, comments, setup/codebooks)
     * libvorbis expects parsed Ogg pages, but we'll fake them with our raw data instead */
    vorbis_info_init(&data->vi);
    vorbis_comment_init(&data->vc);

    data->op.packet = data->buffer;
    data->op.b_o_s = 1; /* fake headers start */

    /* init header */
    switch(data->type) {
        case VORBIS_FSB:    ok = vorbis_custom_setup_init_fsb(streamFile, start_offset, data); break;
        case VORBIS_WWISE:  ok = vorbis_custom_setup_init_wwise(streamFile, start_offset, data); break;
        case VORBIS_OGL:    ok = vorbis_custom_setup_init_ogl(streamFile, start_offset, data); break;
        case VORBIS_SK:     ok = vorbis_custom_setup_init_sk(streamFile, start_offset, data); break;
        case VORBIS_VID1:   ok = vorbis_custom_setup_init_vid1(streamFile, start_offset, data); break;
        default: goto fail;
    }
    if(!ok) goto fail;

    data->op.b_o_s = 0; /* end of fake headers */

    /* init vorbis global and block state */
    if (vorbis_synthesis_init(&data->vd,&data->vi) != 0) goto fail;
    if (vorbis_block_init(&data->vd,&data->vb) != 0) goto fail;


    /* write output */
    config->data_start_offset = data->config.data_start_offset;


    return data;

fail:
    free_vorbis_custom(data);
    return NULL;
}

/* Decodes Vorbis packets into a libvorbis sample buffer, and copies them to outbuf */
void decode_vorbis_custom(VGMSTREAM * vgmstream, sample * outbuf, int32_t samples_to_do, int channels) {
    VGMSTREAMCHANNEL *stream = &vgmstream->ch[0];
    vorbis_custom_codec_data * data = vgmstream->codec_data;
    size_t stream_size =  get_streamfile_size(stream->streamfile);
    //data->op.packet = data->buffer;/* implicit from init */
    int samples_done = 0;

    while (samples_done < samples_to_do) {

        /* extra EOF check for edge cases */
        if (stream->offset >= stream_size) {
            memset(outbuf + samples_done * channels, 0, (samples_to_do - samples_done) * sizeof(sample) * channels);
            break;
        }


        if (data->samples_full) {  /* read more samples */
            int samples_to_get;
            float **pcm;

            /* get PCM samples from libvorbis buffers */
            samples_to_get = vorbis_synthesis_pcmout(&data->vd, &pcm);
            if (!samples_to_get) {
                data->samples_full = 0; /* request more if empty*/
                continue;
            }

            if (data->samples_to_discard) {
                /* discard samples for looping */
                if (samples_to_get > data->samples_to_discard)
                    samples_to_get = data->samples_to_discard;
                data->samples_to_discard -= samples_to_get;
            }
            else {
                /* get max samples and convert from Vorbis float pcm to 16bit pcm */
                if (samples_to_get > samples_to_do - samples_done)
                    samples_to_get = samples_to_do - samples_done;
                pcm_convert_float_to_16(data, outbuf + samples_done * channels, samples_to_get, pcm);
                samples_done += samples_to_get;
            }

            /* mark consumed samples from the buffer
             * (non-consumed samples are returned in next vorbis_synthesis_pcmout calls) */
            vorbis_synthesis_read(&data->vd, samples_to_get);
        }
        else { /* read more data */
            int ok, rc;

            /* not actually needed, but feels nicer */
            data->op.granulepos += samples_to_do; /* can be changed next if desired */
            data->op.packetno++;

            /* read/transform data into the ogg_packet buffer and advance offsets */
            switch(data->type) {
                case VORBIS_FSB:    ok = vorbis_custom_parse_packet_fsb(stream, data); break;
                case VORBIS_WWISE:  ok = vorbis_custom_parse_packet_wwise(stream, data); break;
                case VORBIS_OGL:    ok = vorbis_custom_parse_packet_ogl(stream, data); break;
                case VORBIS_SK:     ok = vorbis_custom_parse_packet_sk(stream, data); break;
                case VORBIS_VID1:   ok = vorbis_custom_parse_packet_vid1(stream, data); break;
                default: goto decode_fail;
            }
            if(!ok) {
                VGM_LOG("Vorbis: cannot parse packet @ around %lx\n",stream->offset);
                goto decode_fail;
            }


            /* parse the fake ogg packet into a logical vorbis block */
            rc = vorbis_synthesis(&data->vb,&data->op);
            if (rc == OV_ENOTAUDIO) {
                VGM_LOG("Vorbis: not an audio packet (size=0x%x) @ %lx\n",(size_t)data->op.bytes,stream->offset);
                //VGM_LOGB(data->op.packet, (size_t)data->op.bytes,0);
                continue; /* seems ok? */
            } else if (rc != 0) {
                VGM_LOG("Vorbis: cannot parse Vorbis block @ %lx\n",stream->offset);
                goto decode_fail;
            }

            /* finally decode the logical block into samples */
            rc = vorbis_synthesis_blockin(&data->vd,&data->vb);
            if (rc != 0)  {
                VGM_LOG("Vorbis: cannot decode Vorbis block @ %lx\n",stream->offset);
                goto decode_fail; /* ? */
            }

            data->samples_full = 1;
        }
    }

    return;

decode_fail:
    /* on error just put some 0 samples */
    memset(outbuf + samples_done * channels, 0, (samples_to_do - samples_done) * channels * sizeof(sample));
}

/* converts from internal Vorbis format to standard PCM (mostly from Xiph's decoder_example.c) */
static void pcm_convert_float_to_16(vorbis_custom_codec_data * data, sample * outbuf, int samples_to_do, float ** pcm) {
    int i,j;

    /* convert float PCM (multichannel float array, with pcm[0]=ch0, pcm[1]=ch1, pcm[2]=ch0, etc)
     * to 16 bit signed PCM ints (host order) and interleave + fix clipping */
    for (i = 0; i < data->vi.channels; i++) {
        sample *ptr = outbuf + i;
        float *mono = pcm[i];
        for (j = 0; j < samples_to_do; j++) {
            int val = (int)floor(mono[j] * 32767.f + .5f);
            if (val > 32767) val = 32767;
            if (val < -32768) val = -32768;

            *ptr = val;
            ptr += data->vi.channels;
        }
    }
}

/* ********************************************** */

void free_vorbis_custom(vorbis_custom_codec_data * data) {
    if (!data)
        return;

    /* internal decoder cleanp */
    vorbis_info_clear(&data->vi);
    vorbis_comment_clear(&data->vc);
    vorbis_dsp_clear(&data->vd);

    free(data->buffer);
    free(data);
}

void reset_vorbis_custom(VGMSTREAM *vgmstream) {
    vorbis_custom_codec_data *data = vgmstream->codec_data;

    /* Seeking is provided by the Ogg layer, so with custom vorbis we'd need seek tables instead.
     * To avoid having to parse different formats we'll just discard until the expected sample */
    vorbis_synthesis_restart(&data->vd);
    data->samples_to_discard = 0;
}

void seek_vorbis_custom(VGMSTREAM *vgmstream, int32_t num_sample) {
    vorbis_custom_codec_data *data = vgmstream->codec_data;

    /* Seeking is provided by the Ogg layer, so with custom vorbis we'd need seek tables instead.
     * To avoid having to parse different formats we'll just discard until the expected sample */
    vorbis_synthesis_restart(&data->vd);
    data->samples_to_discard = num_sample;
    if (vgmstream->loop_ch)
        vgmstream->loop_ch[0].offset = vgmstream->loop_ch[0].channel_start_offset;
}

#endif
