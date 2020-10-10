/*
 * Copyright (C) 2020 Sreerenj Balachandran <bsreerenj@gmail.com>
 *
*/
#include <locale.h>
#include <glib.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>

#ifdef G_OS_UNIX
#include <glib-unix.h>
#endif

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define RTP_PAYLOAD_TYPE "96"
#define RTP_CAPS_VP8 "application/x-rtp,media=video,encoding-name=VP8,payload="

#define SOUP_HTTP_PORT 57776
#define STUN_SERVER "stun.l.google.com:19302"

#define SEND_SRC(pattern) "videotestsrc is-live=true pattern=" pattern " ! timeoverlay ! queue ! vp8enc ! rtpvp8pay ! queue ! " \
    "capsfilter caps=application/x-rtp,media=video,payload=96,encoding-name=VP8"

#define SEND_UDP_SRC() " udpsrc port=5600  !  queue ! " \
    "capsfilter caps=application/x-rtp,media=video,payload=96,encoding-name=H264 !  queue"

//#define SEND_UDP_SRC() " udpsrc port=5600 close-socket=false multicast-iface=false auto-multicast=true !  queue ! " \
//    "capsfilter caps=application/x-rtp,media=video,payload=96,encoding-name=H264 !  queue"

#define SEND_SRC_H264(pattern) "videotestsrc is-live=true pattern=" pattern " ! timeoverlay ! queue ! x264enc ! rtph264pay ! queue ! " \
    "capsfilter caps=application/x-rtp,media=video,payload=96,encoding-name=H264"

#define RECEIVE_H264() "queue ! capsfilter application/x-rtp,media=video,payload=96,encoding-name=H264 ! rtpjitterbuffer ! rtph264depay ! avdec_h264   ! fpsdisplaysink  sync=false async=false --verbose"

#define RECEIVE_VP8() "queue ! rtpvp8depay ! vp8dec  ! videoconvert ! queue ! fpsdisplaysink  sync=false async=false --verbose"

static GMainLoop *loop;
static GstBus *bus;

GMainLoop *mainloop;
SoupServer *soup_server;
GHashTable *receiver_entry_table;

typedef struct _ReceiverEntry ReceiverEntry;

ReceiverEntry *create_receiver_entry (ReceiverEntry * connection);
void destroy_receiver_entry (gpointer receiver_entry_ptr);

GstPadProbeReturn payloader_caps_event_probe_cb (GstPad * pad,
    GstPadProbeInfo * info, gpointer user_data);

void on_offer_created_cb (GstPromise * promise, gpointer user_data);
void on_negotiation_needed_cb (GstElement * webrtcbin, gpointer user_data);
void on_ice_candidate_cb (GstElement * webrtcbin, guint mline_index,
    gchar * candidate, gpointer user_data);

void soup_websocket_message_cb (SoupWebsocketConnection * connection,
    SoupWebsocketDataType data_type, GBytes * message, gpointer user_data);
void soup_websocket_closed_cb (SoupWebsocketConnection * connection,
    gpointer user_data);

void soup_http_handler (SoupServer * soup_server, SoupMessage * message,
    const char *path, GHashTable * query, SoupClientContext * client_context,
    gpointer user_data);
void soup_websocket_handler (G_GNUC_UNUSED SoupServer * server,
    SoupWebsocketConnection * connection, const char *path,
    SoupClientContext * client_context, gpointer user_data);

static gchar *get_string_from_json_object (JsonObject * object);

struct _ReceiverEntry
{
  SoupWebsocketConnection *connection;

  GstElement *pipeline;
  GstElement *webrtcbin;

  /* used to create fake sending pipeline for negotiating
   * inital sdp, ice etc */
  GstElement *extra_src;

  /* parent pipeline is for the initial connection with a client.
   * parent is responsible for receiving the stream from client, postprocess
   * and create a child process to send the processed stream to the client */
  gboolean is_parent;
};


static gboolean
_bus_watch (GstBus * bus, GstMessage * msg, GstElement * pipe)
{
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_STATE_CHANGED:
      /* Debug: To generate the graph of the pipelines, enable the
       * code below and set GST_DEBUG_DUMP_DOT_DIR before running the
       * server application */
#if 0
      if (GST_ELEMENT (msg->src) == pipe) {
        GstState old, new, pending;
        gst_message_parse_state_changed (msg, &old, &new, &pending);

        {
          gchar *dump_name = g_strconcat ("state_changed-",
              gst_element_state_get_name (old), "_",
              gst_element_state_get_name (new), NULL);
	  g_message ("dump_name %s",dump_name);
          GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (msg->src),
              GST_DEBUG_GRAPH_SHOW_ALL, dump_name);
          g_free (dump_name);
        }
      }
#endif
      break;
    case GST_MESSAGE_ERROR:{
      GError *err = NULL;
      gchar *dbg_info = NULL;

      GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipe),
          GST_DEBUG_GRAPH_SHOW_ALL, "error");

      gst_message_parse_error (msg, &err, &dbg_info);
      g_printerr ("ERROR from element %s: %s\n",
          GST_OBJECT_NAME (msg->src), err->message);
      g_printerr ("Debugging info: %s\n", (dbg_info) ? dbg_info : "none");
      g_error_free (err);
      g_free (dbg_info);
      g_main_loop_quit (loop);
      break;
    }
    case GST_MESSAGE_EOS:{
      GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipe),
          GST_DEBUG_GRAPH_SHOW_ALL, "eos");
      g_print ("EOS received\n");
      g_main_loop_quit (loop);
      break;
    }
    default:
      break;
  }

  return TRUE;
}

static void
send_command(SoupWebsocketConnection * connection, gchar *type, gchar *data)
{
  gchar *json_string;
  JsonObject *command_json;
  JsonObject *command_data_json;

  command_json = json_object_new ();
  json_object_set_string_member (command_json, "type", type);

  //ToDo: Can be used for secure random id based registration
#if 0
  command_data_json = json_object_new ();
  json_object_set_string_member (command_data_json, "type", "parent_regid");
  json_object_set_string_member (command_data_json, "value", "S101");
  json_object_set_object_member (command_json, "data", command_data_json);
#endif  

  json_string = get_string_from_json_object (command_json);
  json_object_unref (command_json);
  soup_websocket_connection_send_text (connection, json_string);
  g_free (json_string);
}

static void
handle_media_stream (GstPad * pad, GstElement * pipe, const char *convert_name,
    const char *sink_name, ReceiverEntry * receiver_entry)
{
  GstPad *qpad;
  GstElement *q, *conv, *resample, *sink, *conv2;
  GstPadLinkReturn ret;

  gst_print ("Trying to handle stream with %s ! %s", convert_name, sink_name);
  g_message ("Trying to handle stream with %s ! %s", convert_name, sink_name);

  q = gst_element_factory_make ("queue", NULL);
  g_assert_nonnull (q);
  conv = gst_element_factory_make (convert_name, NULL);
  g_assert_nonnull (conv);
  sink = gst_element_factory_make (sink_name, NULL);
  g_assert_nonnull (sink);

  if (g_strcmp0 (convert_name, "audioconvert") == 0) {
    /* Might also need to resample, so add it just in case.
     * Will be a no-op if it's not required. */
    resample = gst_element_factory_make ("audioresample", NULL);
    g_assert_nonnull (resample);
    gst_bin_add_many (GST_BIN (pipe), q, conv, resample, sink, NULL);
    gst_element_sync_state_with_parent (q);
    gst_element_sync_state_with_parent (conv);
    gst_element_sync_state_with_parent (resample);
    gst_element_sync_state_with_parent (sink);
    gst_element_link_many (q, conv, resample, sink, NULL);
  } else {
    /***** START: Use udpsink to send the stream to another process ****/
    GstWebRTCRTPTransceiver *trans = NULL;
    GstElement *scale = NULL;
    GstElement *filter = NULL;
    GstElement *extra_src = NULL;
    GstElement *webrtcbin = NULL;
    GstElement *send_source  = NULL;
    GstElement *fqueue1 = NULL;
    GstElement *fqueue2 = NULL;
    GstElement *henc  = NULL;
    GstElement *hpay = NULL;
    GstElement *udpsink = NULL;
    GstElement *facedetect = NULL;
    GstElement *fconvert = NULL;
    GstElement *time_overlay = NULL;
    GstElement *text_overlay = NULL;
    
    time_overlay = gst_element_factory_make ("timeoverlay", NULL);

    text_overlay = gst_element_factory_make ("textoverlay", NULL);
    gst_util_set_object_arg (G_OBJECT(text_overlay),
		    "text","We are Post processing!");
    gst_util_set_object_arg (G_OBJECT(text_overlay), "valignment", "top");
    gst_util_set_object_arg (G_OBJECT(text_overlay), "halignment", "left");
    gst_util_set_object_arg (G_OBJECT(text_overlay), "font-desc", "Sans, 72");
    
    conv2 = gst_element_factory_make ("videoconvert", "videoconvert2");
    scale = gst_element_factory_make ("videoscale", "videoscale");

    filter = gst_element_factory_make ("capsfilter", "fitler");
    gst_util_set_object_arg (G_OBJECT (filter), "caps",
      "video/x-raw, width=320, height=240");
    
    fqueue1 = gst_element_factory_make ("queue", NULL);
    fqueue2 = gst_element_factory_make ("queue", NULL);

    henc = gst_element_factory_make ("vaapih264enc", "h264enc");
    if (!henc) {
      g_message ("Failed to Create H/W h264enc,"
		      "using software which has unknown issue"
		      "in our pipeline if facedetect is enabled");
      henc = gst_element_factory_make ("x264enc", "h264enc");
      g_object_set (henc, "tune", 0x00000004, NULL);
    }

    hpay = gst_element_factory_make ("rtph264pay", "h264pay");
    
    udpsink = gst_element_factory_make ("udpsink", "udpsink");
    g_object_set (udpsink, "host", "127.0.0.1", NULL);
    g_object_set (udpsink, "port", 5600, NULL);
  
    g_assert (henc && hpay && udpsink);

    facedetect = gst_element_factory_make ("facedetect", "opencvfacedetect");
    fconvert = gst_element_factory_make ("videoconvert", "fconvert");

    g_assert (conv2 && facedetect && fconvert && fqueue1 && fqueue2);

    gst_bin_add_many (GST_BIN (pipe), q, conv, time_overlay, text_overlay,scale, filter , conv2, facedetect , fqueue1, fqueue2, fconvert, henc, hpay, udpsink, NULL);

    gst_element_sync_state_with_parent (q);
    gst_element_sync_state_with_parent (conv);
    gst_element_sync_state_with_parent (time_overlay);
    gst_element_sync_state_with_parent (sink);
    gst_element_sync_state_with_parent (text_overlay);
    gst_element_sync_state_with_parent (scale);
    gst_element_sync_state_with_parent (filter);
    gst_element_sync_state_with_parent (conv2);
    gst_element_sync_state_with_parent (fqueue1);
    gst_element_sync_state_with_parent (facedetect);
    gst_element_sync_state_with_parent (fqueue2);
    gst_element_sync_state_with_parent (fconvert);
    gst_element_sync_state_with_parent (henc);
    gst_element_sync_state_with_parent (hpay);
    gst_element_sync_state_with_parent (udpsink);
    gst_element_link_many (q, conv, time_overlay, /*text_overlay,*/ scale, filter, conv2, fqueue1, facedetect ,fqueue2, fconvert , henc, hpay, udpsink, NULL);

    send_command (receiver_entry->connection, "REQUEST_SECOND_CONNECTION", NULL);
    /***** END: Use udpsink to send the stream to another process ****/
  }
  qpad = gst_element_get_static_pad (q, "sink");

  ret = gst_pad_link (pad, qpad);
  g_assert_cmphex (ret, ==, GST_PAD_LINK_OK);

}

static void
on_incoming_decodebin_stream (GstElement * decodebin, GstPad * pad,
		ReceiverEntry * receiver_entry)
{
  GstElement * pipe = receiver_entry->pipeline;
  GstCaps *caps;
  const gchar *name;

  if (!gst_pad_has_current_caps (pad)) {
    gst_printerr ("Pad '%s' has no caps, can't do anything, ignoring\n",
        GST_PAD_NAME (pad));
    return;
  }

  caps = gst_pad_get_current_caps (pad);
  name = gst_structure_get_name (gst_caps_get_structure (caps, 0));

  if (g_str_has_prefix (name, "video")) {
    handle_media_stream (pad, pipe, "videoconvert", "autovideosink", receiver_entry);
  } else if (g_str_has_prefix (name, "audio")) {
    handle_media_stream (pad, pipe, "audioconvert", "autoaudiosink", receiver_entry);
  } else {
    gst_printerr ("Unknown pad %s, ignoring", GST_PAD_NAME (pad));
  }
}

static void
on_incoming_stream (GstElement * webrtc, GstPad * pad,
    ReceiverEntry * receiver_entry)
{
  GstElement *decodebin;
  GstPad *sinkpad;

  if (GST_PAD_DIRECTION (pad) != GST_PAD_SRC)
    return;

  g_message ("Incoming Stream received...............%d",receiver_entry->is_parent);
  
  decodebin = gst_element_factory_make ("decodebin", NULL);
  g_signal_connect (decodebin, "pad-added",
      G_CALLBACK (on_incoming_decodebin_stream), receiver_entry);
  gst_bin_add (GST_BIN (receiver_entry->pipeline), decodebin);
  gst_element_sync_state_with_parent (decodebin);

  sinkpad = gst_element_get_static_pad (decodebin, "sink");
  gst_pad_link (pad, sinkpad);
  gst_object_unref (sinkpad);

}

ReceiverEntry *
create_receiver_entry (ReceiverEntry *receiver_entry)
{
  GError *error;
  GstCaps *video_caps;
  GstWebRTCRTPTransceiver *trans = NULL;
  GArray *transceivers;
  SoupWebsocketConnection * connection = NULL;
  GstPad *extra_src_pad = NULL;
  GstPad *webrtcpad = NULL;
  GstElement *webbin = NULL;
  GstElement *udpsrc = NULL;
  GstElement *rtpjitterbuffer = NULL;
  GstElement *rtpcapsfilter = NULL;
  GstCaps *rtpcaps = NULL;

  if (receiver_entry) {
    connection = receiver_entry->connection;
  } else {
    g_message ("Fixme: CreateReceiverEntry..Connnection=%p ", connection);
    receiver_entry = g_slice_alloc0 (sizeof (ReceiverEntry));
    receiver_entry->connection = connection;

    g_object_ref (G_OBJECT (connection));

    g_signal_connect (G_OBJECT (connection), "message",
      G_CALLBACK (soup_websocket_message_cb), (gpointer) receiver_entry);
  } 
  error = NULL;

  udpsrc = gst_element_factory_make ("udpsrc", "udpsrc");
  rtpjitterbuffer = gst_element_factory_make ("rtpjitterbuffer", "rtpjitterbuffer");
  rtpcapsfilter = gst_element_factory_make ("capsfilter", "rtpcapsfilter");

  g_assert (udpsrc && rtpjitterbuffer);
  g_object_set (udpsrc, "address", "127.0.0.1", NULL);
  g_object_set (udpsrc, "port", 5600, NULL);
  g_object_set (udpsrc, "close-socket", FALSE, NULL);
  g_object_set (udpsrc, "multicast-iface", FALSE, NULL);
  g_object_set (udpsrc, "auto-multicast", TRUE, NULL);


  rtpcaps = gst_caps_new_simple ("application/x-rtp",
		  "encoding-name", G_TYPE_STRING,"H264",
		  "payload", G_TYPE_INT, 96, NULL);  
  g_assert (rtpcaps);
  g_object_set (rtpcapsfilter, "caps", rtpcaps, NULL);

  if (receiver_entry->is_parent) {
  receiver_entry->extra_src =
        gst_parse_bin_from_description (SEND_SRC ("smpte"), TRUE, NULL);

  }
  else {
  g_message ("create secondary sourceeeeeeeeeeeeeeeeeeeeeeeeeeee");
  receiver_entry->extra_src =
        gst_parse_bin_from_description (SEND_UDP_SRC (), TRUE, NULL);
  //receiver_entry->extra_src =
    //    gst_parse_bin_from_description (SEND_SRC_H264 ("snow"), TRUE, NULL);

#if 0
    /* decode & display the send stream, working */
    {
      GstElement *display_pipeline = gst_parse_bin_from_description (RECEIVE_VP8(), TRUE, NULL);
      g_assert (display_pipeline);
      receiver_entry->pipeline = gst_pipeline_new ("pipeline");
      gst_bin_add_many (GST_BIN (receiver_entry->pipeline), receiver_entry->extra_src, display_pipeline, NULL);
      gst_element_link_many (receiver_entry->extra_src, display_pipeline, NULL);
      gst_element_set_state (receiver_entry->pipeline, GST_STATE_PLAYING);
      g_message ("local depay, decode & render pipeline.................................................");
      return receiver_entry;
    }
#endif
  }

  receiver_entry->pipeline = gst_pipeline_new ("pipeline");

  receiver_entry->webrtcbin = gst_element_factory_make ("webrtcbin", "webrtcbin");

  if (!receiver_entry->extra_src || !receiver_entry->pipeline || !receiver_entry->webrtcbin) {
  	  exit(0);
  }

  g_assert (receiver_entry->pipeline && receiver_entry->webrtcbin);

  g_object_set (receiver_entry->pipeline, "message-forward", TRUE, NULL);

  bus = gst_pipeline_get_bus (GST_PIPELINE (receiver_entry->pipeline));

  gst_bus_add_watch (bus, (GstBusFunc) _bus_watch, receiver_entry->pipeline);

  g_object_set (receiver_entry->webrtcbin, "bundle-policy", 3, NULL);
  g_object_set (receiver_entry->webrtcbin, "stun-server", "stun://stun.l.google.com:19302", NULL);


  gst_bin_add_many (GST_BIN (receiver_entry->pipeline), receiver_entry->extra_src, receiver_entry->webrtcbin, NULL);
  //gst_bin_add_many (GST_BIN (receiver_entry->pipeline), udpsrc, rtpjitterbuffer, rtpcapsfilter, receiver_entry->webrtcbin, NULL);

  //gst_element_sync_state_with_parent (receiver_entry->extra_src);
  //extra_src_pad = receiver_entry->extra_src->srcpads->data;
  //if (!extra_src_pad) {
  //	  g_message ("Failed to get extra source pad");
  //	  exit(0);
  //}

  gst_element_link_many (receiver_entry->extra_src, receiver_entry->webrtcbin, NULL);
  //gst_element_link_many (udpsrc, rtpjitterbuffer, rtpcapsfilter, receiver_entry->webrtcbin, NULL);
#if 0
  webrtcpad = gst_element_get_request_pad (receiver_entry->webrtcbin, "sink%d");
  if (!webrtcpad) {
	  g_message ("Failed to get webrtc sinkpad");
	  exit(0);
  }
  gst_pad_link (extra_src_pad, webrtcpad);
#endif


  /* Incoming streams will be exposed via this signal */
  if (receiver_entry->is_parent) {
  g_signal_connect (receiver_entry->webrtcbin, "pad-added",
      G_CALLBACK (on_incoming_stream), receiver_entry);
  } else {

    g_signal_emit_by_name (receiver_entry->webrtcbin, "get-transceivers",
        &transceivers);
    g_assert (transceivers != NULL && transceivers->len > 0);
    trans = g_array_index (transceivers, GstWebRTCRTPTransceiver *, 0);
    trans->direction = GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY;
    g_array_unref (transceivers);
  }
  g_signal_connect (receiver_entry->webrtcbin, "on-negotiation-needed",
      G_CALLBACK (on_negotiation_needed_cb), (gpointer) receiver_entry);

  g_signal_connect (receiver_entry->webrtcbin, "on-ice-candidate",
      G_CALLBACK (on_ice_candidate_cb), (gpointer) receiver_entry);

  gst_element_set_state (receiver_entry->pipeline, GST_STATE_PLAYING);

  return receiver_entry;

cleanup:
  destroy_receiver_entry ((gpointer) receiver_entry);
  return NULL;
}

void
destroy_receiver_entry (gpointer receiver_entry_ptr)
{
  ReceiverEntry *receiver_entry = (ReceiverEntry *) receiver_entry_ptr;

  g_assert (receiver_entry != NULL);

  if (receiver_entry->pipeline != NULL) {
    gst_element_set_state (GST_ELEMENT (receiver_entry->pipeline),
        GST_STATE_NULL);

    gst_object_unref (GST_OBJECT (receiver_entry->webrtcbin));
    gst_object_unref (GST_OBJECT (receiver_entry->pipeline));
  }

  if (receiver_entry->connection != NULL)
    g_object_unref (G_OBJECT (receiver_entry->connection));

  g_slice_free1 (sizeof (ReceiverEntry), receiver_entry);
}

void
on_offer_created_cb (GstPromise * promise, gpointer user_data)
{
  gchar *sdp_string;
  gchar *json_string;
  gchar *text;
  JsonObject *sdp_json;
  JsonObject *sdp_data_json;
  GstStructure const *reply;
  GstPromise *local_desc_promise;
  GstWebRTCSessionDescription *offer = NULL;
  ReceiverEntry *receiver_entry = (ReceiverEntry *) user_data;

  reply = gst_promise_get_reply (promise);
  gst_structure_get (reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
      &offer, NULL);
  gst_promise_unref (promise);

  local_desc_promise = gst_promise_new ();
  g_signal_emit_by_name (receiver_entry->webrtcbin, "set-local-description",
      offer, local_desc_promise);
  gst_promise_interrupt (local_desc_promise);
  gst_promise_unref (local_desc_promise);

  sdp_string = gst_sdp_message_as_text (offer->sdp);
  gst_print ("Negotiation offer created:\n%s\n", sdp_string);

  sdp_json = json_object_new ();
  json_object_set_string_member (sdp_json, "type", "sdp");

  sdp_data_json = json_object_new ();
  json_object_set_string_member (sdp_data_json, "type", "offer");
  json_object_set_string_member (sdp_data_json, "sdp", sdp_string);
  json_object_set_object_member (sdp_json, "data", sdp_data_json);

  json_string = get_string_from_json_object (sdp_json);
  json_object_unref (sdp_json);

  soup_websocket_connection_send_text (receiver_entry->connection, json_string);
  g_free (json_string);
  g_free (sdp_string);

  gst_webrtc_session_description_free (offer);

#if 0
  if (offer->type == GST_WEBRTC_SDP_TYPE_OFFER) {
    gst_print ("Sending offer:\n%s\n", text);
    json_object_set_string_member (sdp_json, "type", "offer");
  } else if (offer->type == GST_WEBRTC_SDP_TYPE_ANSWER) {
    gst_print ("Sending answer:\n%s\n", text);
    json_object_set_string_member (sdp_json, "type", "answer");
  } else {
    g_assert_not_reached ();
  }

  json_object_set_string_member (sdp_json, "sdp", sdp_string);
  
   //g_free (sdp_string);
  //json_object_set_string_member (sdp_json, "type", "sdp");

  sdp_data_json = json_object_new ();
  json_object_set_object_member (sdp_data_json, "sdp", sdp_json);
  text = get_string_from_json_object (sdp_data_json);
  json_object_unref (sdp_data_json);

  //json_object_set_string_member (sdp_data_json, "type", "offer");
  //json_object_set_string_member (sdp_data_json, "sdp", sdp_string);
  //json_object_set_object_member (sdp_json, "data", sdp_data_json);

  //json_string = get_string_from_json_object (sdp_json);
  //json_object_unref (sdp_json);

  //soup_websocket_connection_send_text (receiver_entry->connection, json_string);
  soup_websocket_connection_send_text (receiver_entry->connection, text);
  //g_free (text);
  //g_free (json_string);
  //g_free (sdp_string);

  gst_webrtc_session_description_free (offer);
#endif
}


void
on_negotiation_needed_cb (GstElement * webrtcbin, gpointer user_data)
{
  GstPromise *promise;
  ReceiverEntry *receiver_entry = (ReceiverEntry *) user_data;

  gst_print ("Creating negotiation offer........ %d\n",receiver_entry->is_parent);

  promise = gst_promise_new_with_change_func (on_offer_created_cb,
      (gpointer) receiver_entry, NULL);
  g_signal_emit_by_name (G_OBJECT (webrtcbin), "create-offer", NULL, promise);
}

void
on_ice_candidate_cb (G_GNUC_UNUSED GstElement * webrtcbin, guint mline_index,
    gchar * candidate, gpointer user_data)
{
  JsonObject *ice_json;
  JsonObject *ice_data_json;
  gchar *json_string;
  ReceiverEntry *receiver_entry = (ReceiverEntry *) user_data;

  gst_print ("on ice candiate cb.............. %d\n",receiver_entry->is_parent);
  ice_json = json_object_new ();
  json_object_set_string_member (ice_json, "type", "ice");

  ice_data_json = json_object_new ();
  json_object_set_int_member (ice_data_json, "sdpMLineIndex", mline_index);
  json_object_set_string_member (ice_data_json, "candidate", candidate);
  json_object_set_object_member (ice_json, "data", ice_data_json);

  json_string = get_string_from_json_object (ice_json);
  json_object_unref (ice_json);

  soup_websocket_connection_send_text (receiver_entry->connection, json_string);
  g_free (json_string);
}


void
soup_websocket_message_cb (G_GNUC_UNUSED SoupWebsocketConnection * connection,
    SoupWebsocketDataType data_type, GBytes * message, gpointer user_data)
{
  gsize size;
  gchar *data;
  gchar *data_string;
  const gchar *type_string;
  JsonNode *root_json;
  JsonObject *root_json_object;
  JsonObject *data_json_object;
  JsonParser *json_parser = NULL;
  ReceiverEntry *receiver_entry = (ReceiverEntry *) user_data;

  g_message ("TTT: ===========WebSocketReceived message============= %d, connection %p \n",receiver_entry->is_parent, connection);

  switch (data_type) {
    case SOUP_WEBSOCKET_DATA_BINARY:
      g_error ("Received unknown binary message, ignoring\n");
      g_bytes_unref (message);
      return;

    case SOUP_WEBSOCKET_DATA_TEXT:
      data = g_bytes_unref_to_data (message, &size);
      /* Convert to NULL-terminated string */
      data_string = g_strndup (data, size);
      g_free (data);
      break;

    default:
      g_assert_not_reached ();
  }

  g_message ("message received from client %s", data_string);

  json_parser = json_parser_new ();
  if (!json_parser_load_from_data (json_parser, data_string, -1, NULL))
    goto unknown_message;

  root_json = json_parser_get_root (json_parser);
  if (!JSON_NODE_HOLDS_OBJECT (root_json))
    goto unknown_message;

  root_json_object = json_node_get_object (root_json);
  
  if (json_object_has_member (root_json_object, "REPLY_CONNECTION_TYPE")) {
    const gchar *reply_type_string;
    reply_type_string = json_object_get_string_member (
		    root_json_object, "REPLY_CONNECTION_TYPE");

    if (!g_strcmp0 (reply_type_string, "parent"))
      receiver_entry->is_parent = TRUE;
    else
      receiver_entry->is_parent = FALSE;

    g_message ("Reply Type String === %s is_parent=%d", reply_type_string,receiver_entry->is_parent);

    receiver_entry = create_receiver_entry (receiver_entry);

    g_hash_table_replace (receiver_entry_table, connection, receiver_entry);
    goto cleanup;
  }

  if (!json_object_has_member (root_json_object, "type")) {
    g_error ("Received message without type field\n");
    goto cleanup;
  }
  type_string = json_object_get_string_member (root_json_object, "type");

  if (!json_object_has_member (root_json_object, "data")) {
    g_error ("Received message without data field\n");
    goto cleanup;
  }
  data_json_object = json_object_get_object_member (root_json_object, "data");

  if (g_strcmp0 (type_string, "sdp") == 0) {
    const gchar *sdp_type_string;
    const gchar *sdp_string;
    GstPromise *promise;
    GstSDPMessage *sdp;
    GstWebRTCSessionDescription *answer;
    int ret;

    if (!json_object_has_member (data_json_object, "type")) {
      g_error ("Received SDP message without type field\n");
      goto cleanup;
    }
    sdp_type_string = json_object_get_string_member (data_json_object, "type");

    if (g_strcmp0 (sdp_type_string, "answer") != 0) {
      g_error ("Expected SDP message type \"answer\", got \"%s\"\n",
          sdp_type_string);
      goto cleanup;
    }

    if (!json_object_has_member (data_json_object, "sdp")) {
      g_error ("Received SDP message without SDP string\n");
      goto cleanup;
    }
    sdp_string = json_object_get_string_member (data_json_object, "sdp");

    gst_print ("Received SDP:\n%s\n", sdp_string);

    ret = gst_sdp_message_new (&sdp);
    g_assert_cmphex (ret, ==, GST_SDP_OK);

    ret =
        gst_sdp_message_parse_buffer ((guint8 *) sdp_string,
        strlen (sdp_string), sdp);
    if (ret != GST_SDP_OK) {
      g_error ("Could not parse SDP string\n");
      goto cleanup;
    }

    answer = gst_webrtc_session_description_new (GST_WEBRTC_SDP_TYPE_ANSWER,
        sdp);
    g_assert_nonnull (answer);

    promise = gst_promise_new ();
    g_signal_emit_by_name (receiver_entry->webrtcbin, "set-remote-description",
        answer, promise);
    gst_promise_interrupt (promise);
    gst_promise_unref (promise);
    //gst_webrtc_session_description_free (answer);
  } else if (g_strcmp0 (type_string, "ice") == 0) {
    guint mline_index;
    const gchar *candidate_string;

    if (!json_object_has_member (data_json_object, "sdpMLineIndex")) {
      g_error ("Received ICE message without mline index\n");
      goto cleanup;
    }
    mline_index =
        json_object_get_int_member (data_json_object, "sdpMLineIndex");

    if (!json_object_has_member (data_json_object, "candidate")) {
      g_error ("Received ICE message without ICE candidate string\n");
      goto cleanup;
    }
    candidate_string = json_object_get_string_member (data_json_object,
        "candidate");

    gst_print ("Received ICE candidate with mline index %u; candidate: %s\n",
        mline_index, candidate_string);

    g_signal_emit_by_name (receiver_entry->webrtcbin, "add-ice-candidate",
        mline_index, candidate_string);
  } else
    goto unknown_message;

cleanup:
  if (json_parser != NULL)
    g_object_unref (G_OBJECT (json_parser));
  g_free (data_string);
  return;

unknown_message:
  g_error ("Unknown message \"%s\", ignoring", data_string);
  goto cleanup;
}


void
soup_websocket_closed_cb (SoupWebsocketConnection * connection,
    gpointer user_data)
{
  GHashTable *receiver_entry_table = (GHashTable *) user_data;
  g_hash_table_remove (receiver_entry_table, connection);
  gst_print ("Closed websocket connection %p\n", (gpointer) connection);
}


void
soup_http_handler (G_GNUC_UNUSED SoupServer * soup_server,
    SoupMessage * message, const char *path, G_GNUC_UNUSED GHashTable * query,
    G_GNUC_UNUSED SoupClientContext * client_context,
    G_GNUC_UNUSED gpointer user_data)
{
  SoupBuffer *soup_buffer;
  char *html_response = "Access-Control-Allow-Origin: *\n";
  if ((g_strcmp0 (path, "/") != 0) && (g_strcmp0 (path, "/index.html") != 0)) {
    soup_message_set_status (message, SOUP_STATUS_NOT_FOUND);
    return;
  }

  g_message ("path %s",path);
  g_message ("html_response %s",html_response);
  soup_buffer =
      soup_buffer_new (SOUP_MEMORY_STATIC, html_response, strlen (html_response));

  //soup_message_headers_set_content_type (message->response_headers, "text/html",
  //    NULL);
  soup_message_body_append_buffer (message->response_body, soup_buffer);
  soup_buffer_free (soup_buffer);

  soup_message_set_status (message, SOUP_STATUS_OK);
}


void
soup_websocket_handler (G_GNUC_UNUSED SoupServer * server,
    SoupWebsocketConnection * connection, G_GNUC_UNUSED const char *path,
    G_GNUC_UNUSED SoupClientContext * client_context, gpointer user_data)
{
  ReceiverEntry *receiver_entry = NULL;
  GHashTable *receiver_entry_table = (GHashTable *) user_data;
  static int k  =0;

  g_signal_connect (G_OBJECT (connection), "closed",
      G_CALLBACK (soup_websocket_closed_cb), (gpointer) receiver_entry_table);

  receiver_entry = g_slice_alloc0 (sizeof (ReceiverEntry));
  receiver_entry->connection = connection;
  g_object_ref (G_OBJECT (connection));

  g_message ("Processing web socket connection...%p",connection);

  g_signal_connect (G_OBJECT (connection), "message",
      G_CALLBACK (soup_websocket_message_cb), (gpointer) receiver_entry);

  send_command(connection, "REQUEST_CONNECTION_TYPE", NULL);
}

static gchar *
get_string_from_json_object (JsonObject * object)
{
  JsonNode *root;
  JsonGenerator *generator;
  gchar *text;

  /* Make it the root node */
  root = json_node_init_object (json_node_alloc (), object);
  generator = json_generator_new ();
  json_generator_set_root (generator, root);
  text = json_generator_to_data (generator, NULL);

  /* Release everything */
  g_object_unref (generator);
  json_node_free (root);
  return text;
}

#ifdef G_OS_UNIX
gboolean
exit_sighandler (gpointer user_data)
{
  gst_print ("Caught signal, stopping mainloop\n");
  GMainLoop *mainloop = (GMainLoop *) user_data;
  g_main_loop_quit (mainloop);
  return TRUE;
}
#endif

int
main (int argc, char *argv[])
{

  setlocale (LC_ALL, "");
  gst_init (&argc, &argv);

  receiver_entry_table =
      g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
      destroy_receiver_entry);

  mainloop = g_main_loop_new (NULL, FALSE);
  g_assert (mainloop != NULL);

#ifdef G_OS_UNIX
  g_unix_signal_add (SIGINT, exit_sighandler, mainloop);
  g_unix_signal_add (SIGTERM, exit_sighandler, mainloop);
#endif

  soup_server =
      soup_server_new (SOUP_SERVER_SERVER_HEADER, "webrtc-soup-server", NULL);
  soup_server_add_handler (soup_server, "/", soup_http_handler, NULL, NULL);
  soup_server_add_websocket_handler (soup_server, "/ws", NULL, NULL,
      soup_websocket_handler, (gpointer) receiver_entry_table, NULL);
  soup_server_listen_all (soup_server, SOUP_HTTP_PORT,
      (SoupServerListenOptions) 0, NULL);

  gst_print ("WebRTC page link: http://127.0.0.1:%d/\n", (gint) SOUP_HTTP_PORT);

  g_main_loop_run (mainloop);

  g_object_unref (G_OBJECT (soup_server));
  g_hash_table_destroy (receiver_entry_table);
  g_main_loop_unref (mainloop);

  gst_deinit ();

  return 0;
}
