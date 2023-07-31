// SPDX-License-Identifier: LGPL-2.0-or-later

#include <stdbool.h>
#include <gst/gst.h>
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"
#include "mpeg_packet.h"

/**
 * SECTION:element-plugin
 *
 * FIXME:Describe plugin here.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 filesrc location=video.mpg ! mpegpsdemux name=demux ! mpegvideoparse ! avdec_mpeg2video ! queue ! autovideosink \
 *                                             demux. ! mpegaudioparse ! avdec_mp2float ! audioconvert ! queue ! autoaudiosink
 * gst-launch-1.0 filesrc location=video.mpg ! krkr_demux name=demux ! krkr_video ! queue ! autovideosink \
 *                                             demux. ! avdec_mp2float ! audioconvert ! queue ! autoaudiosink
 * ]|
 * </refsect2>
 */

GST_DEBUG_CATEGORY_STATIC(gst_krkr_debug);
#define GST_CAT_DEFAULT gst_krkr_debug

// in GStreamer, decoding a mpeg video requires three elements: mpegpsdemux ! mpegvideoparse ! avdec_mpeg2video
// in pl_mpeg, the latter two are merged to one object
// in DirectShow, the FORMER two are merged to one object
// (similar for audio, the elements are named mpegpsdemux ! mpegaudioparse ! avdec_mp2float)

// In both cases, the middle element takes arbitrary-sized chunks, adds video size and sample rate and stuff to the output pad,
//   discards corrupted data, and returns proper packets.
// Wine's CLSID_CMpegAudioCodec corresponds to avdec_mp2float only, so I implemented my own mpegaudioparse.
//   It's trivial (other than error recovery, which I just ignore - file corruption isn't a thing anymore).
// However, mpegvideoparse is complicated, and Wine doesn't implement CLSID_CMpegVideoCodec,
//   so I chose to split data differently.

// rank rules: Must be higher than media-converter, and lower than every relevant official GStreamer filter.
// The relevant filters are
// - protonaudioconverter     MARGINAL    audio/x-wma
// - protonaudioconverterbin  MARGINAL+1  audio/x-wma
// - protonvideoconverter     MARGINAL    video/x-ms-asf, video/x-msvideo, video/mpeg, video/quicktime
// - mpegpsdemux              PRIMARY     video/mpeg(systemstream=true)
// - mpegvideoparse           PRIMARY+1   video/mpeg(systemstream=false)
// - avdec_mpeg2video         PRIMARY     video/mpeg(mpegversion=[1,2], systemstream=false, parsed=false)
// - mpegaudioparse           PRIMARY+2   audio/mpeg(mpegversion=1, layer=2)
// - avdec_mp2float           MARGINAL    audio/mpeg(mpegversion=1, layer=2, parsed=true)
// - asfdemux                 SECONDARY   video/x-ms-asf
// - avdec_wma*               MARGINAL    audio/x-wma
// - avdec_wmv*               MARGINAL    video/x-wmv

// This means I must use at least rank MARGINAL+2 for audio/x-wma, and MARGINAL+1 for the others.
// However, I also need to rank below avdec_wma*'s MARGINAL. Obviously, that's impossible.
// To solve this, I will create two elements; krkrwma and krkrwmaauto.
// Former is a normal decoder, with rank MARGINAL-1. Latter has rank MARGINAL+2, and is just a wrapper thing.
//    Upon creation, it reads the GStreamer registry and looks for anything else that can read audio/wma.
//    It ignores itself, subtracts 16 points from protonaudioconverter and protonaudioconverterbin,
//    and returns the one with highest adjusted rank.
// Ideally, krkrwmaauto wouldn't even exist if protonaudioconverter doesn't, but that's just infeasible.
// avdec_mp2float is also MARGINAL, but neither I nor protonaudioconverter accept audio/mpeg, so there's no problem there.

// Sources are mostly gst-inspect-1.0; some source code is available at
// <https://github.com/ValveSoftware/Proton/blob/proton_8.0/media-converter/src/videoconv/mod.rs#L44>
// <https://github.com/ValveSoftware/Proton/blob/proton_8.0/media-converter/src/videoconv/imp.rs#L483C52-L487C76>
// <https://github.com/GStreamer/gst-plugins-bad/blob/master/gst/mpegdemux/gstmpegdemux.c#L224>
// <https://github.com/GStreamer/gst-plugins-bad/blob/master/gst/videoparsers/gstmpegvideoparse.c#L69>
// <https://github.com/GStreamer/gst-libav/blob/4f649c9556ce5b4501363885995554598303e01c/ext/libav/gstavviddec.c#L2566>

//#define DEMUX_RANK GST_RANK_MARGINAL+2
//#define VIDEO_RANK GST_RANK_MARGINAL+2

#define DEMUX_RANK GST_RANK_PRIMARY+2
#define VIDEO_RANK GST_RANK_PRIMARY+2

static void print_event(const char * pad_name, GstEvent* event)
{
	//return;
	fprintf(stderr, "gstkrkr: Received %s event on %s\n", GST_EVENT_TYPE_NAME(event), pad_name);
	return;
	gst_print("gstkrkr: Received %s event on %s: ", GST_EVENT_TYPE_NAME(event), pad_name);
	
	switch (GST_EVENT_TYPE(event))
	{
	case GST_EVENT_CAPS:
	{
		GstCaps* caps;
		gst_event_parse_caps(event, &caps);
		gst_print("%" GST_PTR_FORMAT, caps);
		break;
	}
	case GST_EVENT_SEGMENT:
	{
		const GstSegment * segment;
		gst_event_parse_segment(event, &segment);
		gst_print("%lu/%lu\n", (unsigned long)segment->position, (unsigned long)segment->duration);
		break;
	}
	case GST_EVENT_TAG:
	{
		GstTagList* taglist;
		gst_event_parse_tag(event, &taglist);
		gst_print("%" GST_PTR_FORMAT, taglist);
		break;
	}
	default:
		gst_print("(unknown type)");
	}
}

static void print_query(const char * pad_name, GstQuery* query)
{
	//return;
	fprintf(stderr, "gstkrkr: Received %s query on %s\n", GST_QUERY_TYPE_NAME(query), pad_name);
}

// same as gst_buffer_new_memdup, except that function is new in GStreamer 1.20; Proton's is version 1.18.5
static GstBuffer* wrap_bytes_to_gst(const uint8_t * buf, size_t size)
{
	GBytes* by = g_bytes_new(buf, size);
	GstBuffer* ret = gst_buffer_new_wrapped_bytes(by);
	g_bytes_unref(by);
	return ret;
}





#define GST_TYPE_PLMPEG_DEMUX (gst_krkr_demux_get_type())
G_DECLARE_FINAL_TYPE(GstPlMpegDemux, gst_krkr_demux, GST, PLMPEG_DEMUX, GstElement)

struct _GstPlMpegDemux
{
	GstElement element;
	
	GstPad* sinkpad;
	GstPad* videopad;
	GstPad* audiopad;
	
	plm_buffer_t* buf;
	plm_demux_t* demux;
	
	size_t audio_chunk_pos;
	uint8_t audio_chunk[MP2_PACKET_MAX_SIZE];
};

static GstStaticPadTemplate demux_sink_factory = GST_STATIC_PAD_TEMPLATE(
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS("video/mpeg, mpegversion=(int)1, systemstream=(boolean)true")
	);

static GstStaticPadTemplate demux_videosrc_factory = GST_STATIC_PAD_TEMPLATE(
	"video",
	GST_PAD_SRC,
	GST_PAD_SOMETIMES,
	GST_STATIC_CAPS("video/mpeg, mpegversion=(int)1, systemstream=(boolean)false, parsed=(boolean)true, pixel-aspect-ratio=(fraction)1/1")
	//video/mpeg, width=(int)640, height=(int)360, framerate=(fraction)30000/1001, codec_data=(buffer)000001b328016814ffffe018
	);

static GstStaticPadTemplate demux_audiosrc_factory = GST_STATIC_PAD_TEMPLATE(
	"audio",
	GST_PAD_SRC,
	GST_PAD_SOMETIMES,
	GST_STATIC_CAPS("audio/mpeg, mpegversion=(int)1, mpegaudioversion=(int)1, layer=(int)2, parsed=(boolean)true")
	//audio/mpeg, mpegversion=(int)1, mpegaudioversion=(int)1, layer=(int)2, rate=(int)44100, channels=(int)2, parsed=(boolean)true
	);

G_DEFINE_TYPE(GstPlMpegDemux, gst_krkr_demux, GST_TYPE_ELEMENT);

GST_ELEMENT_REGISTER_DECLARE(krkr_demux);
GST_ELEMENT_REGISTER_DEFINE(krkr_demux, "krkr_demux", DEMUX_RANK, GST_TYPE_PLMPEG_DEMUX);

static void gst_krkr_demux_finalize(GObject* object);

static gboolean gst_krkr_demux_sink_event(GstPad* pad, GstObject* parent, GstEvent* event);
static GstFlowReturn gst_krkr_demux_sink_chain(GstPad* pad, GstObject* parent, GstBuffer* buf);
static gboolean gst_krkr_demux_videosrc_event(GstPad* pad, GstObject* parent, GstEvent* event);
static gboolean gst_krkr_demux_audiosrc_event(GstPad* pad, GstObject* parent, GstEvent* event);
static gboolean gst_krkr_demux_videosrc_query(GstPad* pad, GstObject* parent, GstQuery* query);
static gboolean gst_krkr_demux_audiosrc_query(GstPad* pad, GstObject* parent, GstQuery* query);

static void gst_krkr_demux_class_init(GstPlMpegDemuxClass* klass)
{
	GObjectClass* gobject_class;
	GstElementClass* gstelement_class;
	
	gobject_class = (GObjectClass*)klass;
	gstelement_class = (GstElementClass*)klass;
	
	gobject_class->finalize = gst_krkr_demux_finalize;
	
	gst_element_class_set_details_simple(gstelement_class,
		"krkr_demux",
		"Demuxer",
		"MPEG-1 demuxer",
		"Sir Walrus sir@walrus.se");
	
	gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&demux_sink_factory));
	gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&demux_videosrc_factory));
	gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&demux_audiosrc_factory));
}

static void gst_krkr_demux_init(GstPlMpegDemux* filter)
{
	filter->sinkpad = gst_pad_new_from_static_template(&demux_sink_factory, "sink");
	gst_pad_set_event_function(filter->sinkpad, GST_DEBUG_FUNCPTR(gst_krkr_demux_sink_event));
	gst_pad_set_chain_function(filter->sinkpad, GST_DEBUG_FUNCPTR(gst_krkr_demux_sink_chain));
	GST_OBJECT_FLAG_SET(filter->sinkpad, GST_PAD_FLAG_NEED_PARENT);
	gst_element_add_pad(GST_ELEMENT(filter), filter->sinkpad);
	
	filter->videopad = NULL;
	filter->audiopad = NULL;
	
	// plm_buffer_create_with_capacity would be more memory friendly, but I can't figure out how that interacts with seeking
	filter->buf = plm_buffer_create_for_appending(PLM_BUFFER_DEFAULT_SIZE);
	filter->demux = plm_demux_create(filter->buf, false);
	
	filter->audio_chunk_pos = 0;
	
	//fprintf(stderr, "gstkrkr: Created a demuxer\n");
}

static void gst_krkr_demux_finalize(GObject* object)
{
	GstPlMpegDemux* filter = GST_PLMPEG_DEMUX(object);
	
	// don't unref the pads, the floating ref was taken by gst_element_add_pad
	
	plm_buffer_destroy(filter->buf);
	plm_demux_destroy(filter->demux);
	
	G_OBJECT_CLASS(gst_krkr_demux_parent_class)->finalize(object);
}

static gboolean gst_krkr_demux_sink_event(GstPad* pad, GstObject* parent, GstEvent* event)
{
	print_event("demux sink", event);
	
	GstPlMpegDemux* filter = GST_PLMPEG_DEMUX(parent);
	
	switch (GST_EVENT_TYPE(event))
	{
	case GST_EVENT_CAPS:
		gst_event_unref(event); // don't care what we're given
		return TRUE;
	case GST_EVENT_EOS:
		plm_buffer_signal_end(filter->buf);
		gst_krkr_demux_sink_chain(pad, parent, NULL);
		return TRUE;
	default:
		return gst_pad_event_default(pad, parent, event);
	}
}

static void gst_krkr_demux_process_audio_bytes(GstPlMpegDemux* filter, const uint8_t * buf, size_t len);
static void gst_krkr_demux_process_audio_packet(GstPlMpegDemux* filter, const uint8_t * buf, size_t len);

static GstFlowReturn gst_krkr_demux_sink_chain(GstPad* pad, GstObject* parent, GstBuffer* buf)
{
	GstPlMpegDemux* filter = GST_PLMPEG_DEMUX(parent);
	if (buf != NULL)
	{
		//fprintf(stderr, "gstkrkr: demux %lu bytes\n", gst_buffer_get_size(buf));
		for (size_t n=0;n<gst_buffer_n_memory(buf);n++)
		{
			GstMapInfo meminf;
			GstMemory* mem = gst_buffer_get_memory(buf, n);
			mem = gst_memory_make_mapped(mem, &meminf, GST_MAP_READ);
			plm_buffer_write(filter->buf, meminf.data, meminf.size);
			gst_memory_unref(mem);
		}
	}
	
	while (true)
	{
		plm_packet_t* pack = plm_demux_decode(filter->demux);
		if (!pack)
			break;
		
		if (pack->type == PLM_DEMUX_PACKET_VIDEO_1)
		{
			if (!filter->videopad)
			{
				plm_video_t* vid = plm_video_create_with_buffer(plm_buffer_create_with_memory(pack->data, pack->length, false), true);
				int width = plm_video_get_width(vid);
				int height = plm_video_get_height(vid);
				double fps = plm_video_get_framerate(vid);
				plm_video_destroy(vid);
				
				filter->videopad = gst_pad_new_from_static_template(&demux_videosrc_factory, "video");
				gst_pad_set_event_function(filter->videopad, GST_DEBUG_FUNCPTR(gst_krkr_demux_videosrc_event));
				gst_pad_set_query_function(filter->videopad, GST_DEBUG_FUNCPTR(gst_krkr_demux_videosrc_query));
				GST_OBJECT_FLAG_SET(filter->videopad, GST_PAD_FLAG_NEED_PARENT);
				gst_element_add_pad(GST_ELEMENT(filter), filter->videopad);
				
				gst_pad_push_event(filter->videopad, gst_pad_get_sticky_event(filter->sinkpad, GST_EVENT_STREAM_START, 0));
				GstCaps* videocaps = gst_caps_new_simple("video/mpeg",
					"mpegversion", G_TYPE_INT, 1,
					"systemstream", G_TYPE_BOOLEAN, false,
					"parsed", G_TYPE_BOOLEAN, true,
					"width", G_TYPE_INT, width,
					"height", G_TYPE_INT, height,
					"pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
					"framerate", GST_TYPE_FRACTION, (int)(fps*1000), 1000,
					//"codec_data", GST_TYPE_BUFFER, NULL, // todo
					NULL);
				gst_pad_push_event(filter->videopad, gst_event_new_caps(videocaps));
				
				GstSegment* seg = gst_segment_new();
				gst_segment_init(seg, GST_FORMAT_TIME);
				// don't bother with duration, can't find one without reading the entire file and we don't have it yet
				// (and calling plm_demux_get_duration does weird things to the current packet)
				gst_pad_push_event(filter->videopad, gst_event_new_segment(seg));
			}
			GstBuffer* buf = wrap_bytes_to_gst(pack->data, pack->length);
			gst_pad_push(filter->videopad, buf);
		}
		if (pack->type == PLM_DEMUX_PACKET_AUDIO_1)
			gst_krkr_demux_process_audio_bytes(filter, pack->data, pack->length);
	}
	
	if (plm_demux_has_ended(filter->demux))
	{
		if (filter->videopad != NULL)
			gst_pad_push_event(filter->videopad, gst_event_new_eos());
		if (filter->audiopad != NULL)
		{
			if (filter->audio_chunk_pos > 0)
				gst_krkr_demux_process_audio_packet(filter, filter->audio_chunk, filter->audio_chunk_pos);
			gst_pad_push_event(filter->audiopad, gst_event_new_eos());
		}
		return GST_FLOW_EOS;
	}
	
	return GST_FLOW_OK;
}

static size_t min_sz(size_t a, size_t b) { return a < b ? a : b; }

static void gst_krkr_demux_process_audio_bytes(GstPlMpegDemux* filter, const uint8_t * buf, size_t len)
{
again:
	if (!len)
		return;
	
	size_t claim = min_sz(len, MP2_PACKET_MAX_SIZE - filter->audio_chunk_pos);
	memcpy(filter->audio_chunk + filter->audio_chunk_pos, buf, claim);
	filter->audio_chunk_pos += claim;
	buf += claim;
	len -= claim;
	
	size_t pack_size = SIZE_MAX;
	if (mp2_packet_parse(filter->audio_chunk, filter->audio_chunk_pos, NULL, NULL, &pack_size) < 0)
	{
		GST_ERROR("gstkrkr: corrupt data");
		return;
	}
	if (pack_size <= filter->audio_chunk_pos)
	{
		gst_krkr_demux_process_audio_packet(filter, filter->audio_chunk, pack_size);
		memmove(filter->audio_chunk, filter->audio_chunk+pack_size, filter->audio_chunk_pos-pack_size);
		filter->audio_chunk_pos -= pack_size;
	}
	goto again;
}

static void gst_krkr_demux_process_audio_packet(GstPlMpegDemux* filter, const uint8_t * buf, size_t len)
{
	if (!filter->audiopad)
	{
		int channels;
		int samplerate;
		mp2_packet_parse(buf, len, &samplerate, &channels, NULL);
		
		filter->audiopad = gst_pad_new_from_static_template(&demux_audiosrc_factory, "audio");
		gst_pad_set_event_function(filter->audiopad, GST_DEBUG_FUNCPTR(gst_krkr_demux_audiosrc_event));
		gst_pad_set_query_function(filter->audiopad, GST_DEBUG_FUNCPTR(gst_krkr_demux_audiosrc_query));
		GST_OBJECT_FLAG_SET(filter->audiopad, GST_PAD_FLAG_NEED_PARENT);
		gst_element_add_pad(GST_ELEMENT(filter), filter->audiopad);
		
		gst_pad_push_event(filter->audiopad, gst_pad_get_sticky_event(filter->sinkpad, GST_EVENT_STREAM_START, 0));
		GstCaps* audiocaps = gst_caps_new_simple("audio/mpeg",
			"mpegversion", G_TYPE_INT, 1,
			"mpegaudioversion", G_TYPE_INT, 1,
			"layer", G_TYPE_INT, 2,
			"parsed", G_TYPE_BOOLEAN, true,
			"rate", G_TYPE_INT, samplerate,
			"channels", G_TYPE_INT, channels,
			NULL);
		gst_pad_push_event(filter->audiopad, gst_event_new_caps(audiocaps));
		
		GstSegment* seg = gst_segment_new();
		gst_segment_init(seg, GST_FORMAT_TIME);
		gst_pad_push_event(filter->audiopad, gst_event_new_segment(seg));
	}
	gst_pad_push(filter->audiopad, wrap_bytes_to_gst(buf, len));
}

static gboolean gst_krkr_demux_src_seek(GstPad* pad, GstPlMpegDemux* filter, GstEvent* event)
{
	double rate; // 1.0
	GstFormat format; // GST_FORMAT_TIME
	GstSeekFlags flags; // GST_SEGMENT_FLAG_RESET
	GstSeekType start_type; // GST_SEEK_TYPE_SET
	int64_t start; // 0
	GstSeekType stop_type; // GST_SEEK_TYPE_NONE
	int64_t stop; // 0
	gst_event_parse_seek(event, &rate, &format, &flags, &start_type, &start, &stop_type, &stop);
	
	if (start_type == GST_SEEK_TYPE_SET && start == 0)
	{
		plm_demux_rewind(filter->demux);
	}
	else if (format == GST_FORMAT_TIME)
	{
		int type = (filter->videopad ? PLM_DEMUX_PACKET_VIDEO_1 : PLM_DEMUX_PACKET_AUDIO_1);
		plm_demux_seek(filter->demux, start / 1000000000.0, type, false);
	}
	else return FALSE;
	return TRUE;
}

static gboolean gst_krkr_demux_videosrc_event(GstPad* pad, GstObject* parent, GstEvent* event)
{
	GstPlMpegDemux* filter = GST_PLMPEG_DEMUX(parent);
	
	switch (GST_EVENT_TYPE(event)) {
	case GST_EVENT_CAPS:
		return TRUE;
	case GST_EVENT_SEEK:
		return gst_krkr_demux_src_seek(pad, filter, event);
	default:
		break;
	}
	return gst_pad_event_default(pad, parent, event);
}

static gboolean gst_krkr_demux_audiosrc_event(GstPad* pad, GstObject* parent, GstEvent* event)
{
	GstPlMpegDemux* filter = GST_PLMPEG_DEMUX(parent);
	
	switch (GST_EVENT_TYPE(event)) {
	case GST_EVENT_CAPS:
		return TRUE;
	case GST_EVENT_SEEK:
		return gst_krkr_demux_src_seek(pad, filter, event);
	default:
		break;
	}
	return gst_pad_event_default(pad, parent, event);
}

static gboolean gst_krkr_demux_src_query(GstPad* pad, GstObject* parent, GstQuery* query)
{
	switch (GST_QUERY_TYPE(query))
	{
	case GST_QUERY_DURATION:
	{
		GstFormat fmt;
		gst_query_parse_duration(query, &fmt, NULL);
		if (fmt == GST_FORMAT_TIME)
		{
			gst_query_set_duration(query, GST_FORMAT_TIME, 5000000000); // 5 seconds
			//gst_query_set_duration(query, GST_FORMAT_TIME, );
		}
		return TRUE;
	}
	default:
		break;
	}
	return gst_pad_query_default(pad, parent, query);
}

static gboolean gst_krkr_demux_videosrc_query(GstPad* pad, GstObject* parent, GstQuery* query)
{
	print_query("demux videosrc", query);
	return gst_krkr_demux_src_query(pad, parent, query);
}

static gboolean gst_krkr_demux_audiosrc_query(GstPad* pad, GstObject* parent, GstQuery* query)
{
	print_query("demux audiosrc", query);
	return gst_krkr_demux_src_query(pad, parent, query);
}





#define GST_TYPE_PLMPEG_DECODE (gst_krkr_video_get_type())
G_DECLARE_FINAL_TYPE(GstPlMpegDecode, gst_krkr_video, GST, PLMPEG_DECODE, GstElement)

struct _GstPlMpegDecode
{
	GstElement element;
	
	GstPad* sinkpad;
	GstPad* srcpad;
	
	int width;
	int height;
	
	plm_buffer_t* buf;
	plm_video_t* decode;
};

static GstStaticPadTemplate decode_sink_factory = GST_STATIC_PAD_TEMPLATE(
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS("video/mpeg, mpegversion=(int)1, systemstream=(boolean)false, parsed=(boolean)true")
	);

static GstStaticPadTemplate decode_src_factory = GST_STATIC_PAD_TEMPLATE(
	"src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS("video/x-raw, format=(string)YV12")
	);

G_DEFINE_TYPE(GstPlMpegDecode, gst_krkr_video, GST_TYPE_ELEMENT);

GST_ELEMENT_REGISTER_DECLARE(krkr_video);
// same rank as the demuxer
GST_ELEMENT_REGISTER_DEFINE(krkr_video, "krkr_video", VIDEO_RANK, GST_TYPE_PLMPEG_DECODE);

static void gst_krkr_video_finalize(GObject* object);

static gboolean gst_krkr_video_sink_event(GstPad* pad, GstObject* parent, GstEvent* event);
static GstFlowReturn gst_krkr_video_sink_chain(GstPad* pad, GstObject* parent, GstBuffer* buf);
static gboolean gst_krkr_video_src_event(GstPad* pad, GstObject* parent, GstEvent* event);
static gboolean gst_krkr_video_src_query(GstPad* pad, GstObject* parent, GstQuery* query);

static void gst_krkr_video_class_init(GstPlMpegDecodeClass* klass)
{
	GObjectClass* gobject_class;
	GstElementClass* gstelement_class;
	
	gobject_class = (GObjectClass*)klass;
	gstelement_class = (GstElementClass*)klass;
	
	gobject_class->finalize = gst_krkr_video_finalize;
	
	gst_element_class_set_details_simple(gstelement_class,
		"krkr_video",
		"Decoder/Video",
		"MPEG-1 decoder",
		"Sir Walrus sir@walrus.se");
	
	gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&decode_sink_factory));
	gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&decode_src_factory));
}

static void gst_krkr_video_init(GstPlMpegDecode* filter)
{
	filter->sinkpad = gst_pad_new_from_static_template(&decode_sink_factory, "sink");
	gst_pad_set_event_function(filter->sinkpad, GST_DEBUG_FUNCPTR(gst_krkr_video_sink_event));
	gst_pad_set_chain_function(filter->sinkpad, GST_DEBUG_FUNCPTR(gst_krkr_video_sink_chain));
	GST_OBJECT_FLAG_SET(filter->sinkpad, GST_PAD_FLAG_NEED_PARENT);
	gst_element_add_pad(GST_ELEMENT(filter), filter->sinkpad);
	
	filter->srcpad = gst_pad_new_from_static_template(&decode_src_factory, "src");
	gst_pad_set_event_function(filter->srcpad, GST_DEBUG_FUNCPTR(gst_krkr_video_src_event));
	gst_pad_set_query_function(filter->srcpad, GST_DEBUG_FUNCPTR(gst_krkr_video_src_query));
	GST_OBJECT_FLAG_SET(filter->srcpad, GST_PAD_FLAG_NEED_PARENT);
	gst_element_add_pad(GST_ELEMENT(filter), filter->srcpad);
	
	filter->buf = plm_buffer_create_with_capacity(PLM_BUFFER_DEFAULT_SIZE);
	filter->decode = plm_video_create_with_buffer(filter->buf, false);
	
	//fprintf(stderr, "gstkrkr: Created a decoder\n");
}

static void gst_krkr_video_finalize(GObject* object)
{
	GstPlMpegDecode* filter = GST_PLMPEG_DECODE(object);
	
	plm_buffer_destroy(filter->buf);
	plm_video_destroy(filter->decode);
	
	G_OBJECT_CLASS(gst_krkr_video_parent_class)->finalize(object);
}

static gboolean gst_krkr_video_sink_event(GstPad* pad, GstObject* parent, GstEvent* event)
{
	print_event("decode sink", event);
	
	GstPlMpegDecode* filter = GST_PLMPEG_DECODE(parent);
	
	switch (GST_EVENT_TYPE(event))
	{
	case GST_EVENT_CAPS:
	{
		GstCaps* caps_in;
		gst_event_parse_caps(event, &caps_in);
		GstStructure* struc = gst_caps_get_structure(caps_in, 0);
		
		// expected input caps are
		// video/mpeg, mpegversion=(int)1, systemstream=(boolean)false, parsed=(boolean)true, width=(int)640, height=(int)360,
		// framerate=(fraction)30000/1001, pixel-aspect-ratio=(fraction)1/1, codec_data=(buffer)000001b328016814ffffe018
		
		// output is
		// video/x-raw, format=(string)YV12, width=(int)640, height=(int)360, interlace-mode=(string)progressive,
		// pixel-aspect-ratio=(fraction)1/1, chroma-site=(string)jpeg, colorimetry=(string)2:0:0:0, framerate=(fraction)30000/1001
		gst_structure_get_int(struc, "width", &filter->width);
		gst_structure_get_int(struc, "height", &filter->height);
		int framerate_n;
		int framerate_d;
		gst_structure_get_fraction(struc, "framerate", &framerate_n, &framerate_d);
		
		GstCaps* caps_out = gst_caps_new_simple("video/x-raw",
			"format", G_TYPE_STRING, "YV12",
			"width", G_TYPE_INT, filter->width,
			"height", G_TYPE_INT, filter->height,
			"framerate", GST_TYPE_FRACTION, framerate_n, framerate_d,
			NULL);
		gst_pad_push_event(filter->srcpad, gst_event_new_caps(caps_out));
		return TRUE;
	}
	default:
		return gst_pad_event_default(pad, parent, event);
	}
}

static GstFlowReturn gst_krkr_video_sink_chain(GstPad* pad, GstObject* parent, GstBuffer* buf)
{
	GstPlMpegDecode* filter = GST_PLMPEG_DECODE(parent);
//fprintf(stderr, "gstkrkr: decode %lu bytes, send %d\n", gst_buffer_get_size(buf), filter->should_send_frames);
	for (size_t n=0;n<gst_buffer_n_memory(buf);n++)
	{
		GstMapInfo meminf;
		GstMemory* mem = gst_buffer_get_memory(buf, n);
		mem = gst_memory_make_mapped(mem, &meminf, GST_MAP_READ);
		plm_buffer_write(filter->buf, meminf.data, meminf.size);
		gst_memory_unref(mem);
	}
	
	while (true)
	{
		plm_frame_t* frame = plm_video_decode(filter->decode);
//fprintf(stderr, "gstkrkr: decoded to %p\n", frame);
		if (!frame)
			break;
		
		size_t buflen = frame->width*frame->height*12/8;
		uint8_t* ptr = g_malloc(buflen);
		GstBuffer* buf = gst_buffer_new_wrapped(ptr, buflen);
		for (int y=0;y<frame->height;y++)
		{
			memcpy(ptr, frame->y.data + frame->y.width*y, frame->width);
			ptr += frame->width;
		}
		for (int y=0;y<frame->height/2;y++)
		{
			memcpy(ptr, frame->cr.data + frame->cr.width*y, frame->width/2);
			ptr += frame->width/2;
		}
		for (int y=0;y<frame->height/2;y++)
		{
			memcpy(ptr, frame->cb.data + frame->cb.width*y, frame->width/2);
			ptr += frame->width/2;
		}
		
		buf->pts = frame->time * 1000000000;
		buf->dts = frame->time * 1000000000;
		buf->duration = 1000000000 / plm_video_get_framerate(filter->decode);
//fprintf(stderr, "gstkrkr: send frame, %lu bytes\n", gst_buffer_get_size(buf));
		gst_pad_push(filter->srcpad, buf);
	}
	
	return GST_FLOW_OK;
}

static gboolean gst_krkr_video_src_event(GstPad* pad, GstObject* parent, GstEvent* event)
{
	print_event("decode source", event);
	
	//GstPlMpegDecode* filter = GST_PLMPEG_DECODE(parent);
	
	switch (GST_EVENT_TYPE(event)) {
	case GST_EVENT_CAPS:
		return TRUE;
	default:
		break;
	}
	return gst_pad_event_default(pad, parent, event);
}

static gboolean gst_krkr_video_src_query(GstPad* pad, GstObject* parent, GstQuery* query)
{
	print_query("video src", query);
	return gst_pad_query_default(pad, parent, query);
}





static gboolean plugin_init(GstPlugin* plugin)
{
	GST_DEBUG_CATEGORY_INIT(gst_krkr_debug, "plugin", 0, "krkr plugin");
	return TRUE;
	return GST_ELEMENT_REGISTER(krkr_demux, plugin) && GST_ELEMENT_REGISTER(krkr_video, plugin);
}

#define PACKAGE "krkrwine"
GST_PLUGIN_DEFINE(
  // overriding the version like this makes me a Bad Person(tm), but Proton 8.0 is on 1.18.5, so I need to stay behind
  1, // GST_VERSION_MAJOR,
  18, // GST_VERSION_MINOR,
  G_PASTE(krkr_, PLUGINARCH),
  "krkr PL_MPEG (MPEG-1 decoder) wrapper for GStreamer",
  plugin_init,
  "1.0",
  "LGPL",
  "gstkrkr",
  "https://walrus.se/"
)
