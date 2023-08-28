// SPDX-License-Identifier: LGPL-2.0-or-later

#include <stdbool.h>
#include <gst/gst.h>
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

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
 *                                             demux. ! mpegaudioparse ! avdec_mp2float ! audioconvert ! queue ! autoaudiosink
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

// Most of those elements (or equivalents thereof) are present in Glorious Eggroll and easy to install.
// avdec_mp2float is gone, but mpg123audiodec works just as well. The only truly missing one is avdec_mpeg2video.
// However, audio/x-wma is troublesome - protonaudioconverterbin is above avdec_wma*.
// I can't change either of them, and I don't want to remove any files from Proton, so I'll have to do something ugly:
//    A fake element whose only job is to create a real WMA decoder, with rank MARGINAL+2.

// Sources are mostly gst-inspect-1.0; some source code is available at
// <https://github.com/ValveSoftware/Proton/blob/proton_8.0/media-converter/src/videoconv/mod.rs#L44>
// <https://github.com/ValveSoftware/Proton/blob/proton_8.0/media-converter/src/videoconv/imp.rs#L483C52-L487C76>
// <https://github.com/GStreamer/gst-plugins-bad/blob/master/gst/mpegdemux/gstmpegdemux.c#L224>
// <https://github.com/GStreamer/gst-plugins-bad/blob/master/gst/videoparsers/gstmpegvideoparse.c#L69>
// <https://github.com/GStreamer/gst-libav/blob/4f649c9556ce5b4501363885995554598303e01c/ext/libav/gstavviddec.c#L2566>

#define VIDEO_RANK GST_RANK_MARGINAL+1 // must be higher than protonvideoconverter (MARGINAL) and lower than avdec_mpeg2video (PRIMARY)
#define FAKEWMA_RANK GST_RANK_MARGINAL+2 // must be higher than protonaudioconverter (MARGINAL) and protonaudioconverterbin (MARGINAL+1)

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

static bool strbegin(const char * a, const char * b)
{
	return !strncmp(a, b, strlen(b));
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
	GObjectClass* gobject_class = G_OBJECT_CLASS(klass);
	GstElementClass* gstelement_class = GST_ELEMENT_CLASS(klass);
	
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
		// or
		// video/mpeg, mpegversion=(int)1, systemstream=(boolean)false, parsed=(boolean)false
		
		// output from avdec_mpeg2video is
		// video/x-raw, format=(string)YV12, width=(int)640, height=(int)360, interlace-mode=(string)progressive,
		// pixel-aspect-ratio=(fraction)1/1, chroma-site=(string)jpeg, colorimetry=(string)2:0:0:0, framerate=(fraction)30000/1001
		// (I omit some of them)
		int framerate_n;
		int framerate_d;
		if (gst_structure_get_int(struc, "width", &filter->width) &&
			gst_structure_get_int(struc, "height", &filter->height) &&
			gst_structure_get_fraction(struc, "framerate", &framerate_n, &framerate_d))
		{
			GstCaps* caps_out = gst_caps_new_simple("video/x-raw",
				"format", G_TYPE_STRING, "YV12",
				"width", G_TYPE_INT, filter->width,
				"height", G_TYPE_INT, filter->height,
				"framerate", GST_TYPE_FRACTION, framerate_n, framerate_d,
				NULL);
			gst_pad_push_event(filter->srcpad, gst_event_new_caps(caps_out));
		}
		else
		{
			filter->width = 0;
		}
		return TRUE;
	}
	case GST_EVENT_SEGMENT:
	{
		if (!filter->width)
			return TRUE;
		// fall through
	}
	default:
		return gst_pad_event_default(pad, parent, event);
	}
}

static GstFlowReturn gst_krkr_video_sink_chain(GstPad* pad, GstObject* parent, GstBuffer* buf)
{
	GstKrkrPlMpegVideo* filter = GST_KRKRPLMPEG_VIDEO(parent);
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
		
		if (!filter->width)
		{
			filter->width = frame->width;
			filter->height = frame->height;
			int framerate_n = plm_video_get_framerate(filter->decode) * 1000;
			int framerate_d = 1000;
			GstCaps* caps_out = gst_caps_new_simple("video/x-raw",
				"format", G_TYPE_STRING, "YV12",
				"width", G_TYPE_INT, filter->width,
				"height", G_TYPE_INT, filter->height,
				"framerate", GST_TYPE_FRACTION, framerate_n, framerate_d,
				NULL);
			gst_pad_push_event(filter->srcpad, gst_event_new_caps(caps_out));
			
			GstEvent* segment = gst_pad_get_sticky_event(filter->sinkpad, GST_EVENT_SEGMENT, 0);
			if (segment)
				gst_pad_push_event(filter->srcpad, segment);
		}
		
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



#define GST_TYPE_KRKR_FAKEWMA_BASE (gst_krkr_fakewma_get_type())
G_DECLARE_DERIVABLE_TYPE(GstKrkrFakeWma, gst_krkr_fakewma, GST, KRKR_FAKEWMA_BASE, GstBin)

struct _GstKrkrFakeWmaClass
{
	GstBinClass parent_class;
	GstStaticPadTemplate sink_template;
};

static GstStaticPadTemplate fakewma_src_factory = GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS("audio/x-raw"));

G_DEFINE_TYPE(GstKrkrFakeWma, gst_krkr_fakewma, GST_TYPE_BIN);

static void gst_krkr_fakewma_class_init(GstKrkrFakeWmaClass* klass) {}

static void gst_krkr_fakewma_class_init_child(GstKrkrFakeWmaClass* klass)
{
	const char * name = g_type_get_qdata(G_OBJECT_CLASS_TYPE(klass), g_quark_from_static_string("krkrwine_fakewma_namecaps"));
	const char * caps = name + strlen(name) + 1;
	
	klass->sink_template = (GstStaticPadTemplate)GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS(caps));
	
	GstElementClass* gstelement_class = GST_ELEMENT_CLASS(klass);
	gst_element_class_set_details_simple(gstelement_class,
		name,
		"Decoder/Audio",
		"Fake WMA decoder, loads another one while avoiding protonaudioconverter",
		"Sir Walrus sir@walrus.se");
	
	gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&klass->sink_template));
	gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&fakewma_src_factory));
}

static GstElement* get_decoder_for(GstCaps* caps)
{
	GList* transforms = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_MARGINAL);
	GList* transforms2 = gst_element_factory_list_filter(transforms, caps, GST_PAD_SINK, FALSE);
	gst_plugin_feature_list_free(transforms);
	GstElement* ret = NULL;
	
	for (int pass=0;pass<2;pass++)
	{
		for (GList* iter = transforms2; iter && !ret; iter = iter->next)
		{
			GstPluginFeature* feat = GST_PLUGIN_FEATURE(iter->data);
			GstElementFactory* fac = GST_ELEMENT_FACTORY(iter->data);
			const char * name = gst_plugin_feature_get_name(feat);
			
			if (strbegin(name, "krkr_fake")) // discard ourselves, that's just recursion
				continue;
			if (pass == 0 && strbegin(name, "proton")) // accept it if nothing else exists, better than crashing...
				continue;
			
			ret = gst_element_factory_create(fac, gst_plugin_feature_get_name(feat));
		}
	}
	gst_plugin_feature_list_free(transforms2);
	return ret;
}

static void gst_krkr_fakewma_init(GstKrkrFakeWma* filter) {}

static void gst_krkr_fakewma_init_child(GstKrkrFakeWma* filter)
{
	//fprintf(stderr, "gstkrkr: Created a fakewma\n");
	
	GstKrkrFakeWmaClass* klass = (GstKrkrFakeWmaClass*)G_OBJECT_GET_CLASS(filter);
	
	GstCaps* caps = gst_static_pad_template_get_caps(&klass->sink_template);
	GstElement* real_decoder = get_decoder_for(caps);
	gst_caps_unref(caps);
	gst_bin_add(GST_BIN(filter), real_decoder);
	
	GstPad* sinkpad = gst_ghost_pad_new("sink", gst_element_get_static_pad(real_decoder, "sink"));
	gst_element_add_pad(GST_ELEMENT(filter), sinkpad);
	
	GstPad* srcpad = gst_ghost_pad_new("src", gst_element_get_static_pad(real_decoder, "src"));
	gst_element_add_pad(GST_ELEMENT(filter), srcpad);
}

static gboolean gst_krkr_fakewma_type_create(GstPlugin* plugin, const char * namecaps)
{
	GType this_type = g_type_register_static_simple(
		GST_TYPE_KRKR_FAKEWMA_BASE,
		g_intern_static_string(namecaps), 
		sizeof(GstKrkrFakeWmaClass),
		(GClassInitFunc)gst_krkr_fakewma_class_init_child,
		sizeof(GstKrkrFakeWma),
		(GInstanceInitFunc)gst_krkr_fakewma_init_child,
		G_TYPE_FLAG_NONE);
	g_type_set_qdata(this_type, g_quark_from_static_string("krkrwine_fakewma_namecaps"), (void*)namecaps);
	
	return gst_element_register(plugin, namecaps, FAKEWMA_RANK, this_type);
}



static gboolean plugin_init(GstPlugin* plugin)
{
	GST_DEBUG_CATEGORY_INIT(gst_krkr_debug, "plugin", 0, "krkrwine plugin");
	return GST_ELEMENT_REGISTER(krkr_video, plugin) &&
		gst_krkr_fakewma_type_create(plugin, "krkr_fakewmav1\0audio/x-wma, wmaversion=(int)1") &&
		gst_krkr_fakewma_type_create(plugin, "krkr_fakewmav2\0audio/x-wma, wmaversion=(int)2") &&
		gst_krkr_fakewma_type_create(plugin, "krkr_fakewmav3\0audio/x-wma, wmaversion=(int)3") &&
		gst_krkr_fakewma_type_create(plugin, "krkr_fakewmalossless\0audio/x-wma, wmaversion=(int)4");
}

#ifndef PLUGINARCH
#error Need to define PLUGINARCH, did you typo the makefile?
#endif
#ifdef i386
#error Must compile with -std=c##, not gnu##
#endif

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
