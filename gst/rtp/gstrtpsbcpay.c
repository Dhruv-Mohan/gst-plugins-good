/*  GStreamer RTP SBC payloader
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gst/audio/audio.h>
#include "gstrtpsbcpay.h"
#include <math.h>
#include <string.h>
#include "gstrtputils.h"

#define RTP_SBC_PAYLOAD_HEADER_SIZE 1
#define DEFAULT_MIN_FRAMES 0
#define RTP_SBC_HEADER_TOTAL (12 + RTP_SBC_PAYLOAD_HEADER_SIZE)

/* BEGIN: Packing for rtp_payload */
#ifdef _MSC_VER
#pragma pack(push, 1)
#endif

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
/* FIXME: this seems all a bit over the top for a single byte.. */
struct rtp_payload
{
  guint8 frame_count:4;
  guint8 rfa0:1;
  guint8 is_last_fragment:1;
  guint8 is_first_fragment:1;
  guint8 is_fragmented:1;
}
#elif G_BYTE_ORDER == G_BIG_ENDIAN
struct rtp_payload
{
  guint8 is_fragmented:1;
  guint8 is_first_fragment:1;
  guint8 is_last_fragment:1;
  guint8 rfa0:1;
  guint8 frame_count:4;
}
#else
#error "Unknown byte order"
#endif

#ifdef _MSC_VER
;
#pragma pack(pop)
#else
__attribute__ ((packed));
#endif
/* END: Packing for rtp_payload */

enum
{
  PROP_0,
  PROP_MIN_FRAMES
};

GST_DEBUG_CATEGORY_STATIC (gst_rtp_sbc_pay_debug);
#define GST_CAT_DEFAULT gst_rtp_sbc_pay_debug

#define parent_class gst_rtp_sbc_pay_parent_class
G_DEFINE_TYPE (GstRtpSBCPay, gst_rtp_sbc_pay, GST_TYPE_RTP_BASE_PAYLOAD);

static GstStaticPadTemplate gst_rtp_sbc_pay_sink_factory =
GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-sbc, "
        "rate = (int) { 16000, 32000, 44100, 48000 }, "
        "channels = (int) [ 1, 2 ], "
        "channel-mode = (string) { mono, dual, stereo, joint }, "
        "blocks = (int) { 4, 8, 12, 16 }, "
        "subbands = (int) { 4, 8 }, "
        "allocation-method = (string) { snr, loudness }, "
        "bitpool = (int) [ 2, 64 ]")
    );

static GstStaticPadTemplate gst_rtp_sbc_pay_src_factory =
GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-rtp, "
        "media = (string) audio,"
        "payload = (int) " GST_RTP_PAYLOAD_DYNAMIC_STRING ", "
        "clock-rate = (int) { 16000, 32000, 44100, 48000 },"
        "encoding-name = (string) SBC")
    );

static void gst_rtp_sbc_pay_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_rtp_sbc_pay_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gint
gst_rtp_sbc_pay_get_frame_len (gint subbands, gint channels,
    gint blocks, gint bitpool, const gchar * channel_mode)
{
  gint len;
  gint join;

  len = 4 + (4 * subbands * channels) / 8;

  if (strcmp (channel_mode, "mono") == 0 || strcmp (channel_mode, "dual") == 0)
    len += ((blocks * channels * bitpool) + 7) / 8;
  else {
    join = strcmp (channel_mode, "joint") == 0 ? 1 : 0;
    len += ((join * subbands + blocks * bitpool) + 7) / 8;
  }

  return len;
}

static gboolean
gst_rtp_sbc_pay_set_caps (GstRTPBasePayload * payload, GstCaps * caps)
{
  GstRtpSBCPay *sbcpay;
  gint rate, subbands, channels, blocks, bitpool;
  gint frame_len;
  const gchar *channel_mode;
  GstStructure *structure;

  sbcpay = GST_RTP_SBC_PAY (payload);

  structure = gst_caps_get_structure (caps, 0);
  if (!gst_structure_get_int (structure, "rate", &rate))
    return FALSE;
  if (!gst_structure_get_int (structure, "channels", &channels))
    return FALSE;
  if (!gst_structure_get_int (structure, "blocks", &blocks))
    return FALSE;
  if (!gst_structure_get_int (structure, "bitpool", &bitpool))
    return FALSE;
  if (!gst_structure_get_int (structure, "subbands", &subbands))
    return FALSE;

  channel_mode = gst_structure_get_string (structure, "channel-mode");
  if (!channel_mode)
    return FALSE;

  frame_len = gst_rtp_sbc_pay_get_frame_len (subbands, channels, blocks,
      bitpool, channel_mode);

  sbcpay->frame_length = frame_len;

  gst_rtp_base_payload_set_options (payload, "audio", TRUE, "SBC", rate);

  GST_DEBUG_OBJECT (payload, "calculated frame length: %d ", frame_len);

  return gst_rtp_base_payload_set_outcaps (payload, NULL);
}

static GstFlowReturn
gst_rtp_sbc_pay_flush_buffers (GstRtpSBCPay * sbcpay)
{
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  guint available;
  guint max_payload;
  GstBuffer *outbuf, *paybuf;
  guint8 *payload_data;
  guint frame_count;
  guint payload_length;
  struct rtp_payload *payload;

  if (sbcpay->frame_length == 0) {
    GST_ERROR_OBJECT (sbcpay, "Frame length is 0");
    return GST_FLOW_ERROR;
  }

  available = gst_adapter_available (sbcpay->adapter);

  max_payload =
      gst_rtp_buffer_calc_payload_len (GST_RTP_BASE_PAYLOAD_MTU (sbcpay) -
      RTP_SBC_PAYLOAD_HEADER_SIZE, 0, 0);

  max_payload = MIN (max_payload, available);
  frame_count = max_payload / sbcpay->frame_length;
  payload_length = frame_count * sbcpay->frame_length;
  if (payload_length == 0)      /* Nothing to send */
    return GST_FLOW_OK;

  outbuf = gst_rtp_buffer_new_allocate (RTP_SBC_PAYLOAD_HEADER_SIZE, 0, 0);

  /* get payload */
  gst_rtp_buffer_map (outbuf, GST_MAP_WRITE, &rtp);

  gst_rtp_buffer_set_payload_type (&rtp, GST_RTP_BASE_PAYLOAD_PT (sbcpay));

  /* write header and copy data into payload */
  payload_data = gst_rtp_buffer_get_payload (&rtp);
  payload = (struct rtp_payload *) payload_data;
  memset (payload, 0, sizeof (struct rtp_payload));
  payload->frame_count = frame_count;

  gst_rtp_buffer_unmap (&rtp);

  paybuf = gst_adapter_take_buffer_fast (sbcpay->adapter, payload_length);
  gst_rtp_copy_meta (GST_ELEMENT_CAST (sbcpay), outbuf, paybuf,
      g_quark_from_static_string (GST_META_TAG_AUDIO_STR));
  outbuf = gst_buffer_append (outbuf, paybuf);

  /* FIXME: what about duration? */
  GST_BUFFER_PTS (outbuf) = sbcpay->timestamp;
  GST_DEBUG_OBJECT (sbcpay, "Pushing %d bytes", payload_length);

  return gst_rtp_base_payload_push (GST_RTP_BASE_PAYLOAD (sbcpay), outbuf);
}

static GstFlowReturn
gst_rtp_sbc_pay_handle_buffer (GstRTPBasePayload * payload, GstBuffer * buffer)
{
  GstRtpSBCPay *sbcpay;
  guint available;

  /* FIXME check for negotiation */

  sbcpay = GST_RTP_SBC_PAY (payload);
  sbcpay->timestamp = GST_BUFFER_PTS (buffer);

  gst_adapter_push (sbcpay->adapter, buffer);

  available = gst_adapter_available (sbcpay->adapter);
  if (available + RTP_SBC_HEADER_TOTAL >=
      GST_RTP_BASE_PAYLOAD_MTU (sbcpay) ||
      (available > (sbcpay->min_frames * sbcpay->frame_length)))
    return gst_rtp_sbc_pay_flush_buffers (sbcpay);

  return GST_FLOW_OK;
}

static gboolean
gst_rtp_sbc_pay_sink_event (GstRTPBasePayload * payload, GstEvent * event)
{
  GstRtpSBCPay *sbcpay = GST_RTP_SBC_PAY (payload);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_EOS:
      gst_rtp_sbc_pay_flush_buffers (sbcpay);
      break;
    default:
      break;
  }

  return GST_RTP_BASE_PAYLOAD_CLASS (parent_class)->sink_event (payload, event);
}

static void
gst_rtp_sbc_pay_finalize (GObject * object)
{
  GstRtpSBCPay *sbcpay = GST_RTP_SBC_PAY (object);

  g_object_unref (sbcpay->adapter);

  GST_CALL_PARENT (G_OBJECT_CLASS, finalize, (object));
}

static void
gst_rtp_sbc_pay_class_init (GstRtpSBCPayClass * klass)
{
  GstRTPBasePayloadClass *payload_class = GST_RTP_BASE_PAYLOAD_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = gst_rtp_sbc_pay_finalize;
  gobject_class->set_property = gst_rtp_sbc_pay_set_property;
  gobject_class->get_property = gst_rtp_sbc_pay_get_property;

  payload_class->set_caps = GST_DEBUG_FUNCPTR (gst_rtp_sbc_pay_set_caps);
  payload_class->handle_buffer =
      GST_DEBUG_FUNCPTR (gst_rtp_sbc_pay_handle_buffer);
  payload_class->sink_event = GST_DEBUG_FUNCPTR (gst_rtp_sbc_pay_sink_event);

  /* properties */
  g_object_class_install_property (G_OBJECT_CLASS (klass),
      PROP_MIN_FRAMES,
      g_param_spec_int ("min-frames", "minimum frame number",
          "Minimum quantity of frames to send in one packet "
          "(-1 for maximum allowed by the mtu)",
          -1, G_MAXINT, DEFAULT_MIN_FRAMES, G_PARAM_READWRITE));

  gst_element_class_add_static_pad_template (element_class,
      &gst_rtp_sbc_pay_sink_factory);
  gst_element_class_add_static_pad_template (element_class,
      &gst_rtp_sbc_pay_src_factory);

  gst_element_class_set_static_metadata (element_class, "RTP packet payloader",
      "Codec/Payloader/Network", "Payload SBC audio as RTP packets",
      "Thiago Sousa Santos <thiagoss@lcc.ufcg.edu.br>");

  GST_DEBUG_CATEGORY_INIT (gst_rtp_sbc_pay_debug, "rtpsbcpay", 0,
      "RTP SBC payloader");
}

static void
gst_rtp_sbc_pay_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstRtpSBCPay *sbcpay;

  sbcpay = GST_RTP_SBC_PAY (object);

  switch (prop_id) {
    case PROP_MIN_FRAMES:
      sbcpay->min_frames = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_rtp_sbc_pay_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstRtpSBCPay *sbcpay;

  sbcpay = GST_RTP_SBC_PAY (object);

  switch (prop_id) {
    case PROP_MIN_FRAMES:
      g_value_set_int (value, sbcpay->min_frames);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_rtp_sbc_pay_init (GstRtpSBCPay * self)
{
  self->adapter = gst_adapter_new ();
  self->frame_length = 0;
  self->timestamp = 0;

  self->min_frames = DEFAULT_MIN_FRAMES;
}

gboolean
gst_rtp_sbc_pay_plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "rtpsbcpay", GST_RANK_NONE,
      GST_TYPE_RTP_SBC_PAY);
}
