/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / FLAC audio decoder filter
 *  based on libFLAC (https://xiph.org/flac/)
 *
 */

#include <gpac/filters.h>
#include <string.h>
#include <stdio.h>

#include "FLAC/stream_decoder.h"

typedef struct
{
	const u8 *data;
	u32 size, pos;
} FlacMemHandle;

typedef struct
{
	GF_FilterPid *ipid, *opid;

	Bool is_playing;
	u32 src_timescale;
	u32 codec_id;

	u32 sample_rate, num_channels, bits_per_sample;
	u64 total_samples;

	u16 *out_buf;
	u32 out_alloc, out_pos;
	Bool has_error;

	FlacMemHandle mem;
} GF_FLACDecCtx;

static FLAC__StreamDecoderWriteStatus flacdec_write_cb(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 *const buffer[], void *client_data)
{
	GF_FLACDecCtx *ctx = (GF_FLACDecCtx *)client_data;
	u32 i, ch, nb_ch = frame->header.channels;
	u32 blocksize = frame->header.blocksize;
	u32 shift = (ctx->bits_per_sample > 16) ? (ctx->bits_per_sample - 16) : 0;
	(void)decoder;

	if (ctx->out_pos + blocksize * nb_ch > ctx->out_alloc)
	{
		ctx->has_error = GF_TRUE;
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	}

	for (i = 0; i < blocksize; i++)
	{
		for (ch = 0; ch < nb_ch; ch++)
		{
			s32 s = buffer[ch][i];
			s16 v = (s16)(shift ? (s >> shift) : s);
			ctx->out_buf[ctx->out_pos++] = (u16)v;
		}
	}
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void flacdec_metadata_cb(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data)
{
	GF_FLACDecCtx *ctx = (GF_FLACDecCtx *)client_data;
	(void)decoder;
	if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO)
	{
		ctx->sample_rate = metadata->data.stream_info.sample_rate;
		ctx->num_channels = metadata->data.stream_info.channels;
		ctx->bits_per_sample = metadata->data.stream_info.bits_per_sample;
		ctx->total_samples = metadata->data.stream_info.total_samples;
	}
}

static void flacdec_error_cb(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
	GF_FLACDecCtx *ctx = (GF_FLACDecCtx *)client_data;
	(void)decoder;
	(void)status;
	ctx->has_error = GF_TRUE;
}

/* memory-backed IO callbacks, mirroring filters/libtiff/tiff_mem_io.h's approach.
 * client_data is the GF_FLACDecCtx (shared with write/metadata/error callbacks),
 * not a bare FlacMemHandle - the handle lives at ctx->mem. */
static FLAC__StreamDecoderReadStatus flacdec_mem_read_cb(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *client_data)
{
	GF_FLACDecCtx *ctx = (GF_FLACDecCtx *)client_data;
	FlacMemHandle *h = &ctx->mem;
	u32 avail = h->size - h->pos;
	u32 to_copy = (u32)*bytes;
	(void)decoder;
	if (avail == 0)
	{
		*bytes = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}
	if (to_copy > avail)
		to_copy = avail;
	memcpy(buffer, h->data + h->pos, to_copy);
	h->pos += to_copy;
	*bytes = to_copy;
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderSeekStatus flacdec_mem_seek_cb(const FLAC__StreamDecoder *decoder, FLAC__uint64 absolute_byte_offset, void *client_data)
{
	GF_FLACDecCtx *ctx = (GF_FLACDecCtx *)client_data;
	FlacMemHandle *h = &ctx->mem;
	(void)decoder;
	if (absolute_byte_offset > h->size)
		return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
	h->pos = (u32)absolute_byte_offset;
	return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

static FLAC__StreamDecoderTellStatus flacdec_mem_tell_cb(const FLAC__StreamDecoder *decoder, FLAC__uint64 *absolute_byte_offset, void *client_data)
{
	GF_FLACDecCtx *ctx = (GF_FLACDecCtx *)client_data;
	FlacMemHandle *h = &ctx->mem;
	(void)decoder;
	*absolute_byte_offset = h->pos;
	return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

static FLAC__StreamDecoderLengthStatus flacdec_mem_length_cb(const FLAC__StreamDecoder *decoder, FLAC__uint64 *stream_length, void *client_data)
{
	GF_FLACDecCtx *ctx = (GF_FLACDecCtx *)client_data;
	FlacMemHandle *h = &ctx->mem;
	(void)decoder;
	*stream_length = h->size;
	return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

static FLAC__bool flacdec_mem_eof_cb(const FLAC__StreamDecoder *decoder, void *client_data)
{
	GF_FLACDecCtx *ctx = (GF_FLACDecCtx *)client_data;
	FlacMemHandle *h = &ctx->mem;
	(void)decoder;
	return (h->pos >= h->size) ? true : false;
}

static GF_Err flacdec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	const GF_PropertyValue *prop;
	GF_FLACDecCtx *ctx = (GF_FLACDecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_CODECID);
	if (!prop)
		return GF_NOT_SUPPORTED;
	ctx->ipid = pid;

	if (!ctx->opid)
	{
		ctx->opid = gf_filter_pid_new(filter);
	}

	// copy properties at init or reconfig
	gf_filter_pid_copy_properties(ctx->opid, ctx->ipid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S16));

	return GF_OK;
}

static GF_Err flacdec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck, *dst_pck;
	u8 *data, *output;
	u32 size, out_size;
	FLAC__StreamDecoder *decoder;
	FLAC__StreamDecoderInitStatus init_status;
	GF_FLACDecCtx *ctx = (GF_FLACDecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);

	if (!data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}

	decoder = FLAC__stream_decoder_new();
	if (!decoder)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}
	FLAC__stream_decoder_set_metadata_respond(decoder, FLAC__METADATA_TYPE_STREAMINFO);

	memset(&ctx->mem, 0, sizeof(ctx->mem));
	ctx->mem.data = data;
	ctx->mem.size = size;

	ctx->sample_rate = ctx->num_channels = ctx->bits_per_sample = 0;
	ctx->total_samples = 0;
	ctx->has_error = GF_FALSE;

	init_status = FLAC__stream_decoder_init_stream(decoder,
		flacdec_mem_read_cb, flacdec_mem_seek_cb, flacdec_mem_tell_cb, flacdec_mem_length_cb, flacdec_mem_eof_cb,
		flacdec_write_cb, flacdec_metadata_cb, flacdec_error_cb, ctx);


	if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[FLAC] init_stream failed: %s\n", FLAC__StreamDecoderInitStatusString[init_status]));
		FLAC__stream_decoder_delete(decoder);
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	if (!FLAC__stream_decoder_process_until_end_of_metadata(decoder))
	{
		FLAC__stream_decoder_finish(decoder);
		FLAC__stream_decoder_delete(decoder);
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_NON_COMPLIANT_BITSTREAM;
	}


	if (!ctx->sample_rate || !ctx->num_channels || !ctx->total_samples)
	{
		FLAC__stream_decoder_finish(decoder);
		FLAC__stream_decoder_delete(decoder);
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	ctx->out_alloc = (u32)(ctx->total_samples * ctx->num_channels);
	ctx->out_pos = 0;
	ctx->out_buf = (u16 *)gf_malloc(sizeof(u16) * ctx->out_alloc);
	if (!ctx->out_buf)
	{
		FLAC__stream_decoder_finish(decoder);
		FLAC__stream_decoder_delete(decoder);
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}

	if (!FLAC__stream_decoder_process_until_end_of_stream(decoder) && !ctx->out_pos)
	{
		gf_free(ctx->out_buf);
		ctx->out_buf = NULL;
		FLAC__stream_decoder_finish(decoder);
		FLAC__stream_decoder_delete(decoder);
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	FLAC__stream_decoder_finish(decoder);
	FLAC__stream_decoder_delete(decoder);


	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(ctx->sample_rate));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(ctx->num_channels));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CHANNEL_LAYOUT, &PROP_LONGUINT((ctx->num_channels == 1) ? GF_AUDIO_CH_FRONT_CENTER : GF_AUDIO_CH_FRONT_LEFT | GF_AUDIO_CH_FRONT_RIGHT));

	out_size = ctx->out_pos * sizeof(u16);
	dst_pck = gf_filter_pck_new_alloc(ctx->opid, out_size, &output);
	if (!dst_pck)
	{
		gf_free(ctx->out_buf);
		ctx->out_buf = NULL;
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}
	memcpy(output, ctx->out_buf, out_size);
	gf_free(ctx->out_buf);
	ctx->out_buf = NULL;

	gf_filter_pck_merge_properties(pck, dst_pck);
	gf_filter_pck_set_dependency_flags(dst_pck, 0);
	gf_filter_pck_send(dst_pck);

	gf_filter_pid_drop_packet(ctx->ipid);
	return GF_EOS;
}

static const GF_FilterCapability FLACDecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_CODECID, GF_CODECID_FLAC),
		CAP_BOOL(GF_CAPS_INPUT_EXCLUDED, GF_PROP_PID_UNFRAMED, GF_TRUE),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister FLACDecoderRegister = {
	.name = "flacdec",
	GF_FS_SET_DESCRIPTION("FLAC decoder")
		GF_FS_SET_HELP("This filter decodes FLAC audio using libFLAC.")
			.private_size = sizeof(GF_FLACDecCtx),
	SETCAPS(FLACDecCaps),
	.configure_pid = flacdec_configure_pid,
	.process = flacdec_process,
};

const GF_FilterRegister * EMSCRIPTEN_KEEPALIVE dynCall_flacdec_register(GF_FilterSession *session)
{
	return &FLACDecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_flacdec(void) {
    gf_filter_auto_register("flacdec", dynCall_flacdec_register);
}
