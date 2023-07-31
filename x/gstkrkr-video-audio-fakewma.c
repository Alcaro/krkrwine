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
 * gst-launch-1.0 filesrc location=video.mpg ! mpegpsdemux name=demux ! krkr_mpegvideo ! queue ! autovideosink \
 *                                             demux. ! krkr_mpegaudio ! audioconvert ! queue ! autoaudiosink
 * ]|
 * </refsect2>
 */

GST_DEBUG_CATEGORY_STATIC(gst_krkr_debug);
#define GST_CAT_DEFAULT gst_krkr_debug

// in GStreamer, decoding a mpeg video requires three elements: mpegpsdemux ! mpegvideoparse ! avdec_mpeg2video
// in pl_mpeg, the latter two are merged to one object
// in DirectShow, the FORMER two are merged to one object
// (similar for audio, the elements are named mpegpsdemux ! mpegaudioparse ! avdec_mp2float)
// luckily, the parser elements just take byte sequences and return packets, consisting of the same bytes
//    (but with packet boundaries at significant locations); stacking two mpegvideoparse elements is useless but harmless

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

// Most of those elements are present in Proton, I only need to reimplement avdec_mpeg2video and avdec_mp2float
//    (and I don't need mpegvideoparse and mpegaudioparse, though I don't mind if they exist).
// I need to go above protonvideoconverter, which means my mpeg2video should use MARGINAL+1.
// For mp2float, the real decoder is MARGINAL, so I need to go below that. Luckily, media-converter doesn't want audio/mpeg.
// The problem is audio/x-wma. I don't need to implement any of them myself, but protonaudioconverterbin is above avdec_wma*.
// I can't change either of them, and I don't want to remove any files from Proton, so I'll have to do something ugly:
//    A fake element whose only job is to create a real WMA decoder, with rank MARGINAL+2.

// Sources are mostly gst-inspect-1.0; some source code is available at
// <https://github.com/ValveSoftware/Proton/blob/proton_8.0/media-converter/src/videoconv/mod.rs#L44>
// <https://github.com/ValveSoftware/Proton/blob/proton_8.0/media-converter/src/videoconv/imp.rs#L483C52-L487C76>
// <https://github.com/GStreamer/gst-plugins-bad/blob/master/gst/mpegdemux/gstmpegdemux.c#L224>
// <https://github.com/GStreamer/gst-plugins-bad/blob/master/gst/videoparsers/gstmpegvideoparse.c#L69>
// <https://github.com/GStreamer/gst-libav/blob/4f649c9556ce5b4501363885995554598303e01c/ext/libav/gstavviddec.c#L2566>

#define VIDEO_RANK GST_RANK_MARGINAL+1
#define AUDIO_RANK GST_RANK_MARGINAL-1
#define FAKEWMA_RANK GST_RANK_MARGINAL+2

//#define VIDEO_RANK GST_RANK_PRIMARY+2
//#define AUDIO_RANK GST_RANK_PRIMARY+2
//#define FAKEWMA_RANK GST_RANK_PRIMARY+2

static void print_event(const char * pad_name, GstEvent* event)
{
	return;
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
	return;
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



#define GST_TYPE_PLMPEG_VIDEO (gst_krkr_video_get_type())
G_DECLARE_FINAL_TYPE(GstKrkrPlMpegVideo, gst_krkr_video, GST, KRKRPLMPEG_VIDEO, GstElement)

struct _GstKrkrPlMpegVideo
{
	GstElement element;
	
	GstPad* sinkpad;
	GstPad* srcpad;
	
	int width;
	int height;
	
	plm_buffer_t* buf;
	plm_video_t* decode;
};

static GstStaticPadTemplate decodevideo_sink_factory = GST_STATIC_PAD_TEMPLATE(
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS("video/mpeg, mpegversion=(int)1, systemstream=(boolean)false")
	);

static GstStaticPadTemplate decodevideo_src_factory = GST_STATIC_PAD_TEMPLATE(
	"src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS("video/x-raw, format=(string)YV12")
	);

G_DEFINE_TYPE(GstKrkrPlMpegVideo, gst_krkr_video, GST_TYPE_ELEMENT);

GST_ELEMENT_REGISTER_DECLARE(krkr_video);
GST_ELEMENT_REGISTER_DEFINE(krkr_video, "krkr_mpegvideo", VIDEO_RANK, GST_TYPE_PLMPEG_VIDEO);

static void gst_krkr_video_finalize(GObject* object);

static gboolean gst_krkr_video_sink_event(GstPad* pad, GstObject* parent, GstEvent* event);
static GstFlowReturn gst_krkr_video_sink_chain(GstPad* pad, GstObject* parent, GstBuffer* buf);
static gboolean gst_krkr_video_src_event(GstPad* pad, GstObject* parent, GstEvent* event);
static gboolean gst_krkr_video_src_query(GstPad* pad, GstObject* parent, GstQuery* query);

static void gst_krkr_video_class_init(GstKrkrPlMpegVideoClass* klass)
{
	GObjectClass* gobject_class;
	GstElementClass* gstelement_class;
	
	gobject_class = (GObjectClass*)klass;
	gstelement_class = (GstElementClass*)klass;
	
	gobject_class->finalize = gst_krkr_video_finalize;
	
	gst_element_class_set_details_simple(gstelement_class,
		"krkr_mpegvideo",
		"Decoder/Video",
		"MPEG-1 video decoder",
		"Sir Walrus sir@walrus.se");
	
	gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&decodevideo_sink_factory));
	gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&decodevideo_src_factory));
}

static void gst_krkr_video_init(GstKrkrPlMpegVideo* filter)
{
	filter->sinkpad = gst_pad_new_from_static_template(&decodevideo_sink_factory, "sink");
	gst_pad_set_event_function(filter->sinkpad, GST_DEBUG_FUNCPTR(gst_krkr_video_sink_event));
	gst_pad_set_chain_function(filter->sinkpad, GST_DEBUG_FUNCPTR(gst_krkr_video_sink_chain));
	GST_OBJECT_FLAG_SET(filter->sinkpad, GST_PAD_FLAG_NEED_PARENT);
	gst_element_add_pad(GST_ELEMENT(filter), filter->sinkpad);
	
	filter->srcpad = gst_pad_new_from_static_template(&decodevideo_src_factory, "src");
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
	GstKrkrPlMpegVideo* filter = GST_KRKRPLMPEG_VIDEO(object);
	
	plm_buffer_destroy(filter->buf);
	plm_video_destroy(filter->decode);
	
	G_OBJECT_CLASS(gst_krkr_video_parent_class)->finalize(object);
}

static gboolean gst_krkr_video_sink_event(GstPad* pad, GstObject* parent, GstEvent* event)
{
	print_event("video sink", event);
	
	GstKrkrPlMpegVideo* filter = GST_KRKRPLMPEG_VIDEO(parent);
	
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
	GstKrkrPlMpegVideo* filter = GST_KRKRPLMPEG_VIDEO(parent);
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
	print_event("video source", event);
	
	//GstKrkrPlMpegVideo* filter = GST_PLMPEG_VIDEO(parent);
	
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





#define GST_TYPE_PLMPEG_AUDIO (gst_krkr_audio_get_type())
G_DECLARE_FINAL_TYPE(GstKrkrPlMpegAudio, gst_krkr_audio, GST, KRKRPLMPEG_AUDIO, GstElement)

struct _GstKrkrPlMpegAudio
{
	GstElement element;
	
	GstPad* sinkpad;
	GstPad* srcpad;
	
	plm_buffer_t* buf;
	plm_audio_t* decode;
};

static GstStaticPadTemplate decodeaudio_sink_factory = GST_STATIC_PAD_TEMPLATE(
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS("audio/mpeg, mpegversion=(int)1, layer=(int)2")
	);

static GstStaticPadTemplate decodeaudio_src_factory = GST_STATIC_PAD_TEMPLATE(
	"src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS("audio/x-raw, format=(string)F32LE")
	);

G_DEFINE_TYPE(GstKrkrPlMpegAudio, gst_krkr_audio, GST_TYPE_ELEMENT);

GST_ELEMENT_REGISTER_DECLARE(krkr_audio);
GST_ELEMENT_REGISTER_DEFINE(krkr_audio, "krkr_mpegaudio", AUDIO_RANK, GST_TYPE_PLMPEG_AUDIO);

static void gst_krkr_audio_finalize(GObject* object);

static gboolean gst_krkr_audio_sink_event(GstPad* pad, GstObject* parent, GstEvent* event);
static GstFlowReturn gst_krkr_audio_sink_chain(GstPad* pad, GstObject* parent, GstBuffer* buf);
static gboolean gst_krkr_audio_src_event(GstPad* pad, GstObject* parent, GstEvent* event);
static gboolean gst_krkr_audio_src_query(GstPad* pad, GstObject* parent, GstQuery* query);

static void gst_krkr_audio_class_init(GstKrkrPlMpegAudioClass* klass)
{
	GObjectClass* gobject_class;
	GstElementClass* gstelement_class;
	
	gobject_class = (GObjectClass*)klass;
	gstelement_class = (GstElementClass*)klass;
	
	gobject_class->finalize = gst_krkr_audio_finalize;
	
	gst_element_class_set_details_simple(gstelement_class,
		"krkr_mpegaudio",
		"Decoder/Audio",
		"MPEG-1 layer 2 audio decoder",
		"Sir Walrus sir@walrus.se");
	
	gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&decodeaudio_sink_factory));
	gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&decodeaudio_src_factory));
}

static void gst_krkr_audio_init(GstKrkrPlMpegAudio* filter)
{
	filter->sinkpad = gst_pad_new_from_static_template(&decodeaudio_sink_factory, "sink");
	gst_pad_set_event_function(filter->sinkpad, GST_DEBUG_FUNCPTR(gst_krkr_audio_sink_event));
	gst_pad_set_chain_function(filter->sinkpad, GST_DEBUG_FUNCPTR(gst_krkr_audio_sink_chain));
	GST_OBJECT_FLAG_SET(filter->sinkpad, GST_PAD_FLAG_NEED_PARENT);
	gst_element_add_pad(GST_ELEMENT(filter), filter->sinkpad);
	
	filter->srcpad = gst_pad_new_from_static_template(&decodeaudio_src_factory, "src");
	gst_pad_set_event_function(filter->srcpad, GST_DEBUG_FUNCPTR(gst_krkr_audio_src_event));
	gst_pad_set_query_function(filter->srcpad, GST_DEBUG_FUNCPTR(gst_krkr_audio_src_query));
	GST_OBJECT_FLAG_SET(filter->srcpad, GST_PAD_FLAG_NEED_PARENT);
	gst_element_add_pad(GST_ELEMENT(filter), filter->srcpad);
	
	filter->buf = plm_buffer_create_with_capacity(PLM_BUFFER_DEFAULT_SIZE);
	filter->decode = plm_audio_create_with_buffer(filter->buf, false);
	
	//fprintf(stderr, "gstkrkr: Created a decoder\n");
}

static void gst_krkr_audio_finalize(GObject* object)
{
	GstKrkrPlMpegAudio* filter = GST_KRKRPLMPEG_AUDIO(object);
	
	plm_buffer_destroy(filter->buf);
	plm_audio_destroy(filter->decode);
	
	G_OBJECT_CLASS(gst_krkr_audio_parent_class)->finalize(object);
}

static gboolean gst_krkr_audio_sink_event(GstPad* pad, GstObject* parent, GstEvent* event)
{
	print_event("audio sink", event);
	
	GstKrkrPlMpegAudio* filter = GST_KRKRPLMPEG_AUDIO(parent);
	
	switch (GST_EVENT_TYPE(event))
	{
	case GST_EVENT_CAPS:
	{
		GstCaps* caps_in;
		gst_event_parse_caps(event, &caps_in);
		GstStructure* struc = gst_caps_get_structure(caps_in, 0);
		
		// expected input caps are
		// audio/mpeg, mpegversion=(int)1, mpegaudioversion=(int)1, layer=(int)2, rate=(int)44100, channels=(int)2, parsed=(boolean)true
		
		// output is
		// audio/x-raw, format=(string)F32LE, rate=(int)44100, channels=(int)2, layout=(string)interleaved
		int samplerate;
		gst_structure_get_int(struc, "rate", &samplerate);
		
		GstCaps* caps_out = gst_caps_new_simple("audio/x-raw",
			"format", G_TYPE_STRING, "F32LE", // actually native endian, but everything is LE these days
			"rate", G_TYPE_INT, samplerate,
			"channels", G_TYPE_INT, 2, // it seems that PL_MPEG always emits dual-channel samples, even if the actual file is mono
			"layout", G_TYPE_STRING, "interleaved",
			NULL);
		gst_pad_push_event(filter->srcpad, gst_event_new_caps(caps_out));
		return TRUE;
	}
	default:
		return gst_pad_event_default(pad, parent, event);
	}
}

static GstFlowReturn gst_krkr_audio_sink_chain(GstPad* pad, GstObject* parent, GstBuffer* buf)
{
	GstKrkrPlMpegAudio* filter = GST_KRKRPLMPEG_AUDIO(parent);
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
		plm_samples_t* samp = plm_audio_decode(filter->decode);
//fprintf(stderr, "gstkrkr: decoded to %p\n", samp);
		if (!samp)
			break;
		
		size_t buflen = samp->count * sizeof(float) * 2;
		GstBuffer* buf = wrap_bytes_to_gst((uint8_t*)samp->interleaved, buflen);
		
		buf->pts = samp->time * 1000000000;
		buf->dts = samp->time * 1000000000;
		buf->duration = 1000000000 / plm_audio_get_samplerate(filter->decode);
//fprintf(stderr, "gstkrkr: send frame, %lu bytes\n", gst_buffer_get_size(buf));
		gst_pad_push(filter->srcpad, buf);
	}
	
	return GST_FLOW_OK;
}

static gboolean gst_krkr_audio_src_event(GstPad* pad, GstObject* parent, GstEvent* event)
{
	print_event("audio source", event);
	
	//GstKrkrPlMpegAudio* filter = GST_PLMPEG_AUDIO(parent);
	
	switch (GST_EVENT_TYPE(event)) {
	case GST_EVENT_CAPS:
		return TRUE;
	default:
		break;
	}
	return gst_pad_event_default(pad, parent, event);
}

static gboolean gst_krkr_audio_src_query(GstPad* pad, GstObject* parent, GstQuery* query)
{
	print_query("audio src", query);
	return gst_pad_query_default(pad, parent, query);
}





#define GST_TYPE_FAKEWMA (gst_krkr_fakewma_get_type())
G_DECLARE_FINAL_TYPE(GstKrkrFakeWma, gst_krkr_fakewma, GST, KRKR_FAKEWMA, GstBin)

struct _GstKrkrFakeWma
{
	GstBin bin;
	
	GstPad* sinkpad;
	GstPad* srcpad;
	
	GstElement* decodebin;
};

static GstStaticPadTemplate fakewma_sink_factory = GST_STATIC_PAD_TEMPLATE(
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS("audio/x-wma")
	);

static GstStaticPadTemplate fakewma_src_factory = GST_STATIC_PAD_TEMPLATE(
	"src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS("audio/x-raw")
	);

G_DEFINE_TYPE(GstKrkrFakeWma, gst_krkr_fakewma, GST_TYPE_BIN);

GST_ELEMENT_REGISTER_DECLARE(krkr_fakewma);
GST_ELEMENT_REGISTER_DEFINE(krkr_fakewma, "krkr_fakewma", FAKEWMA_RANK, GST_TYPE_FAKEWMA);

static void gst_krkr_fakewma_finalize(GObject* object);

static void gst_krkr_fakewma_class_init(GstKrkrFakeWmaClass* klass)
{
	GObjectClass* gobject_class;
	GstElementClass* gstelement_class;
	
	gobject_class = (GObjectClass*)klass;
	gstelement_class = (GstElementClass*)klass;
	
	gst_element_class_set_details_simple(gstelement_class,
		"krkr_fakewma",
		"Decoder/Audio",
		"Fake WMA decoder, loads another one while avoiding protonaudioconverter",
		"Sir Walrus sir@walrus.se");
	
	gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&fakewma_sink_factory));
	gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&fakewma_src_factory));
}

static void valuearray_insert_object(GValueArray* arr, size_t pos, void* object)
{
	GValue val = {};
	g_value_init(&val, G_TYPE_OBJECT);
	g_value_set_object(&val, object);
	g_value_array_insert(arr, pos, &val);
	g_value_unset(&val);
}

static GValueArray* gst_krkr_fakewma_decodebin_sort(GstElement* bin, GstPad* pad, GstCaps* caps, GValueArray* factories, void* udata)
{
	// keeps throwing trash about "g_value_array_new is deprecated: Use 'GArray' instead"
	// don't care, GStreamer uses GValueArray so I need this
	GValueArray* ret = g_value_array_new(factories->n_values);
	size_t n_real = 0;
	size_t n_mediaconverter = 0;
	for (size_t n=0;n<factories->n_values;n++)
	{
		GstElementFactory* factory = g_value_get_object(&factories->values[n]);
		
		// discard ourselves, that's just recursion
		if (!strcmp(gst_plugin_feature_get_name(factory), "krkr_fakewma"))
			continue;
		
		// move mediaconverter to the end (it's better than nothing...)
		bool is_mediaconverter = (!strncmp(gst_plugin_feature_get_name(factory), "proton", strlen("proton")));
		
		if (is_mediaconverter)
			valuearray_insert_object(ret, n_real++, factory);
		else
			valuearray_insert_object(ret, n_real + n_mediaconverter++, factory);
	}
	return ret;
}

static void gst_krkr_fakewma_decodebin_pad_added(GstElement* self, GstPad* new_pad, void* user_data)
{
	GstKrkrFakeWma* filter = GST_KRKR_FAKEWMA(user_data);
	
	gst_ghost_pad_set_target(GST_GHOST_PAD(filter->srcpad), new_pad);
}

static void gst_krkr_fakewma_init(GstKrkrFakeWma* filter)
{
	//fprintf(stderr, "gstkrkr: Created a fakewma\n");
	
	filter->decodebin = gst_element_factory_make("decodebin", "decodebin");
	gst_bin_add(GST_BIN(filter), filter->decodebin);
	g_signal_connect(filter->decodebin, "autoplug-sort", G_CALLBACK(gst_krkr_fakewma_decodebin_sort), filter);
	g_signal_connect(filter->decodebin, "pad-added", G_CALLBACK(gst_krkr_fakewma_decodebin_pad_added), filter);
	
	filter->sinkpad = gst_ghost_pad_new("sink", gst_element_get_static_pad(filter->decodebin, "sink"));
	gst_element_add_pad(GST_ELEMENT(filter), filter->sinkpad);
	
	filter->srcpad = gst_ghost_pad_new_no_target_from_template("src", gst_static_pad_template_get(&fakewma_src_factory));
	gst_element_add_pad(GST_ELEMENT(filter), filter->srcpad);
}



static gboolean plugin_init(GstPlugin* plugin)
{
	GST_DEBUG_CATEGORY_INIT(gst_krkr_debug, "plugin", 0, "krkrwine plugin");
	return GST_ELEMENT_REGISTER(krkr_video, plugin) &&
	       //GST_ELEMENT_REGISTER(krkr_audio, plugin) &&
	       GST_ELEMENT_REGISTER(krkr_fakewma, plugin);
}

#define PACKAGE "krkrwine"
GST_PLUGIN_DEFINE(
  // overriding the version like this makes me a Bad Person(tm), but Proton 8.0 is on 1.18.5, so I need to stay behind
  1, // GST_VERSION_MAJOR,
  18, // GST_VERSION_MINOR,
  G_PASTE(krkr_, PLUGINARCH),
  "krkrwine module for GStreamer",
  plugin_init,
  "1.0",
  "LGPL",
  "gstkrkr",
  "https://walrus.se/"
)
