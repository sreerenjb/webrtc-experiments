#Author: Sreerenj Balachandran <bsreerenj@gmail.com>

import random
import ssl
import websockets
import asyncio
import os
import sys
import json
import argparse
import signal

import gi
from gi.repository import GObject
from gi.repository import GLib

gi.require_version('Json', '1.0')
from gi.repository import Json
gi.require_version('Gst', '1.0')
from gi.repository import Gst
gi.require_version('GstWebRTC', '1.0')
from gi.repository import GstWebRTC
gi.require_version('GstSdp', '1.0')
from gi.repository import GstSdp
gi.require_version('Soup', '2.4')
from gi.repository import Gio, Soup

from websockets.version import version as wsv

################## globals #####################
RTP_PAYLOAD_TYPE = "96"
RTP_CAPS_VP8 = "application/x-rtp,media=video,encoding-name=VP8,payload="
SOUP_HTTP_PORT = 57776
STUN_SERVER = "stun.l.google.com:19302"

# hash table to save the connections
receiver_entry_table = {}

SEND_SRC = '''
videotestsrc is-live=true pattern="smpte" ! timeoverlay ! queue ! vp8enc ! rtpvp8pay ! queue ! capsfilter caps=application/x-rtp,media=video,payload=96,encoding-name=VP8
'''
SEND_UDP_SRC = '''
udpsrc port=5600  !  queue ! capsfilter caps=application/x-rtp,media=video,payload=96,encoding-name=VP8 !  queue
'''
# GMainLoop
global_loop = None

# Input Args
global_enable_facedetect = False

################# END (globals) ################

################# Data Structures ####################
class ReceiverEntry:
  def __init__(self, con):
    print ("ReceiverEntryInit: con ",con)
    self.connection = con
    self.pipeline = None
    self.webrtcbin = None
    self.extra_src = None
    self.bus = None
    self.loop = None
    '''
    * parent pipeline is for the initial connection with a client.
    * parent is responsible for receiving the stream from client, postprocess
    * and create a child process to send the processed stream to the client
    '''
    self.is_parent = False

################# END (Data Structures) ##############
def bus_call (bus, message, receiver_entry):
    t = message.type
    if t == Gst.MessageType.EOS:
        sys.stdout.write ("EOS \n")
        receiver_entry.global_loop.quit()
    return True

'''
 (Optional) vaapi => plugin for Intel h/w accelerated elements
 vpx: => vp8 & vp9  encoders & decoders
 pango => plugin for text overlay
 udp   => plugin for udpsink & udpsrc
 videoconvert => plugin for software CSC
 videoscale => plugin for software scaling
'''
def check_plugins():
    needed = ["vpx", "nice", "webrtc", "dtls", "srtp", "rtp",
              "rtpmanager", "videotestsrc", "audiotestsrc", "udp",
              "vpx", "pango", "videoconvert", "videoscale"]
    missing = list(filter(lambda p: Gst.Registry.get().find_plugin(p) is None, needed))
    if len(missing):
        print('Missing gstreamer plugins:', missing)
        return False
    return True


def get_string_from_json_object (json_object):

  print ("get_string_from_json_object: ",json_object)

  root = Json.Node.init_object (Json.Node.alloc(), json_object)
  generator = Json.Generator()
  generator.set_root (root)
  text = generator.to_data ()
  print ("text_from_generator = ",text)
  # Unlike the C api, to_data retuns a tuple
  return text

def send_command (connection, command_type, data):
  
  print ("send_command: ",command_type)

  command_json = Json.Object ()
  command_json.set_string_member ("type", command_type)
  json_string = get_string_from_json_object (command_json)
  connection.send_text (json_string[0])

def soup_websocket_closed_cb (connection, receiver_entry_table):

  print ("Closed websocket connection ", connection)

  del receiver_entry_table [connection]

def on_offer_created_cb (promise, receiver_entry):

  print ("================= on_offer_created_cb ===============")

  reply = promise.get_reply ()
  offer = reply['offer']
  local_desc_promise = Gst.Promise.new()
  receiver_entry.webrtcbin.emit('set-local-description', offer, local_desc_promise)
  local_desc_promise.interrupt()

  sdp_json = Json.Object.new ()
  sdp_json.set_string_member ("type", "sdp")

  sdp_string =offer.sdp.as_text()

  sdp_data_json = Json.Object.new ()
  sdp_data_json.set_string_member ("type", "offer")
  sdp_data_json.set_string_member ("sdp", sdp_string)
 
  sdp_json.set_object_member ("data", sdp_data_json)

  json_string = get_string_from_json_object (sdp_json)

  receiver_entry.connection.send_text (json_string [0])

def on_negotiation_needed_cb (webrtcbin, receiver_entry):
  print ("Creating negotiation offer........is_parent = ",receiver_entry.is_parent)

  promise = Gst.Promise.new_with_change_func (on_offer_created_cb, receiver_entry)
  webrtcbin.emit ("create-offer", None, promise);

def on_ice_candidate_cb (webrtcbin, mline_index, candidate, receiver_entry):

  print ("On ice candiate cb.............. parent = ",receiver_entry.is_parent)

  '''
  icemsg = json.dumps({'ice': {'candidate': candidate, 'sdpMLineIndex': mline_index}})
  loop = asyncio.new_event_loop()
  loop.run_until_complete(receiver_entry.connection.send_text(icemsg))
  loop.close()
  '''
  ice_json = Json.Object.new ()
  ice_json.set_string_member ("type", "ice")

  ice_data_json = Json.Object.new ()
  ice_data_json.set_int_member ("sdpMLineIndex", mline_index)
  ice_data_json.set_string_member ("candidate", candidate)
  ice_json.set_object_member ("data", ice_data_json)

  json_string = get_string_from_json_object (ice_json);

  receiver_entry.connection.send_text (json_string[0])

def handle_media_stream (pad, pipe, convert_name, sink_name, receiver_entry):
  print ("======= Handle media stram =======")

  print ("Trying to handle stream with ", convert_name, " ! ",sink_name)

  q = Gst.ElementFactory.make ("queue")
  assert (q)
  conv = Gst.ElementFactory.make (convert_name)
  assert (conv)
  sink = Gst.ElementFactory.make (sink_name)
  assert (sink)

  if GLib.strcmp0 (convert_name, "audioconvert") == 0:

    resample = Gst.ElementFactory.make ("audioresample")
    assert (resample)
    pipe.add (q)
    pipe.add (conv)
    pipe.add (resample)
    pipe.add (sink)
    q.sync_state_with_parent ()
    conv.sync_state_with_parent ()
    resample.sync_state_with_parent ()
    sinksync_state_with_parent ()
    q.link(conv)
    conv.link(resample)
    resample.link(sink)

  else:

    print ("............Create Video pipeline.................")
    process_element = None

    time_overlay = Gst.ElementFactory.make ("timeoverlay")

    conv2 = Gst.ElementFactory.make ("videoconvert")
    scale = Gst.ElementFactory.make ("videoscale")

    capsfilter = Gst.ElementFactory.make ("capsfilter", "fitler")
    Gst.util_set_object_arg (capsfilter, "caps","video/x-raw, width=320, height=240")

    fqueue1 = Gst.ElementFactory.make ("queue")
    fqueue2 = Gst.ElementFactory.make ("queue")
    fconvert = Gst.ElementFactory.make ("videoconvert")
    assert (conv2  and fconvert and fqueue1 and fqueue2)

    '''
    henc = Gst.ElementFactory.make ("vaapih264enc")
    if henc == None:
      print ("Failed to Create H/W h264enc, using software which has unknown issue in our pipeline if facedetect is enabled")
      henc = Gst.ElementFactory.make ("x264enc")
      Gst.util_set_object_arg (henc, "tune", 0x00000004)

    henc = Gst.ElementFactory.make ("x264enc")
    henc.set_property ("tune", 0x00000004)

    hpay = Gst.ElementFactory.make ("rtph264pay")
    '''

    henc = Gst.ElementFactory.make ("vp8enc")
    hpay = Gst.ElementFactory.make ("rtpvp8pay")

    udpsink = Gst.ElementFactory.make ("udpsink")
    udpsink.set_property ("host", "127.0.0.1")
    udpsink.set_property ("port",5600)

    assert (henc and hpay and udpsink)
    print ("+===============================glboal ",global_enable_facedetect)
    if global_enable_facedetect:
      process_element = Gst.ElementFactory.make ("facedetect")
    else:
      process_element = Gst.ElementFactory.make ("textoverlay")
      Gst.util_set_object_arg (process_element, "text", "We are Post processing!")
      Gst.util_set_object_arg (process_element, "valignment", "bottom")
      Gst.util_set_object_arg (process_element, "halignment", "right")
      Gst.util_set_object_arg (process_element,"font-desc", "Sans, 72")

    assert (process_element)

    pipe.add (q)
    pipe.add (conv)
    pipe.add (time_overlay)
    pipe.add (scale)
    pipe.add (capsfilter)
    pipe.add (conv2)
    pipe.add (process_element)
    pipe.add (fqueue1)
    pipe.add (fqueue2)
    pipe.add (fconvert)
    pipe.add (henc)
    pipe.add (hpay)
    pipe.add (udpsink)

    q.sync_state_with_parent ()
    conv.sync_state_with_parent ()
    time_overlay.sync_state_with_parent ()
    sink.sync_state_with_parent ()
    scale.sync_state_with_parent ()
    capsfilter.sync_state_with_parent ()
    conv2.sync_state_with_parent ()
    fqueue1.sync_state_with_parent ()
    process_element.sync_state_with_parent ()
    fqueue2.sync_state_with_parent ()
    fconvert.sync_state_with_parent ()
    henc.sync_state_with_parent ()
    hpay.sync_state_with_parent ()
    udpsink.sync_state_with_parent ()

    q.link (conv) 
    conv.link (time_overlay)
    time_overlay.link (scale)
    scale.link(capsfilter)
    capsfilter.link(conv2)
    conv2.link(fqueue1)
    fqueue1.link(process_element)
    process_element.link(fqueue2)
    fqueue2.link (fconvert)
    fconvert.link(henc)
    henc.link(hpay)
    hpay.link(udpsink)

    send_command (receiver_entry.connection, "REQUEST_SECOND_CONNECTION", None)
    ##### END: Use udpsink to send the stream to another process ####

  qpad = q.get_static_pad ("sink")

  res = pad.link (qpad)
  assert (res  == Gst.PadLinkReturn.OK)


def on_incoming_decodebin_stream (decodebin, pad, receiver_entry):
  pipe = receiver_entry.pipeline

  print ("=========On incoming decodebin stream==================")
  if pad.has_current_caps () == False:
    print("Warning: No Caps, ignoring")
    return

  caps = pad.get_current_caps ()
  structure = caps.get_structure (0)
  name = structure.get_name ()

  if GLib.str_has_prefix (name, "video") == True:
    handle_media_stream (pad, pipe, "videoconvert", "autovideosink", receiver_entry)
  elif GLib.str_has_prefix (name, "audio") == True:
    handle_media_stream (pad, pipe, "audioconvert", "autoaudiosink", receiver_entry)
  else:
    print ("Unknown pad  ignoring")


def on_incoming_stream (webrtc, pad, receiver_entry):
  print ("=============on_incoming_stream=============== ")

  if pad.direction != Gst.PadDirection.SRC:
    return

  decodebin = Gst.ElementFactory.make ("decodebin")
  decodebin.connect("pad-added", on_incoming_decodebin_stream, receiver_entry)
  receiver_entry.pipeline.add(decodebin)
  decodebin.sync_state_with_parent ()

  sinkpad = decodebin.get_static_pad ("sink")
  pad.link (sinkpad)

  return

def create_receiver_entry (receiver_entry):

  print ("------create receiver entry-------")

  if receiver_entry:
    connection = receiver_entry.connection
  else:
    print ("Fixme: CreateReceiverEntry Without a Connnection!! ")
    return None

  udpsrc = Gst.ElementFactory.make ("udpsrc", "udpsrc")
  rtpjitterbuffer = Gst.ElementFactory.make ("rtpjitterbuffer", "rtpjitterbuffer")
  rtpcapsfilter = Gst.ElementFactory.make ("capsfilter", "rtpcapsfilter")

  assert (udpsrc and rtpjitterbuffer and rtpcapsfilter)

  udpsrc.set_property ("address", "127.0.0.1")
  udpsrc.set_property ("port", 5600)
  udpsrc.set_property ("close-socket", False)
  udpsrc.set_property ("multicast-iface", False)
  udpsrc.set_property ("auto-multicast", True)

  rtpcaps = Gst.Caps.from_string ("application/x-rtp, encoding-name=H264, payload=96")
  assert (rtpcaps)
  
  rtpcapsfilter.set_property ("caps", rtpcaps)

  if receiver_entry.is_parent:
    receiver_entry.extra_src = Gst.parse_bin_from_description (SEND_SRC, True)
  else:
    receiver_entry.extra_src = Gst.parse_bin_from_description (SEND_UDP_SRC, True)

  receiver_entry.pipeline = Gst.Pipeline.new ("pipeline")

  receiver_entry.webrtcbin = Gst.ElementFactory.make ("webrtcbin", "webrtcbin")

  assert (receiver_entry.extra_src and receiver_entry.pipeline and receiver_entry.webrtcbin)

  receiver_entry.webrtcbin.set_property ("bundle-policy", 3)
  receiver_entry.webrtcbin.set_property ("stun-server", "stun://stun.l.google.com:19302")
  receiver_entry.pipeline.set_property ("message-forward", True)

  receiver_entry.bus = receiver_entry.pipeline.get_bus()
  receiver_entry.bus.add_watch (GLib.PRIORITY_DEFAULT, bus_call, receiver_entry)

  receiver_entry.pipeline.add (receiver_entry.extra_src) 
  receiver_entry.pipeline.add (receiver_entry.webrtcbin)
  receiver_entry.extra_src.link(receiver_entry.webrtcbin)

  # Incoming streams will be exposed via this signal
  if receiver_entry.is_parent:
    receiver_entry.webrtcbin.connect ("pad-added", on_incoming_stream, receiver_entry)
  else:
    trans = receiver_entry.webrtcbin.emit ("get-transceiver", 0)
    assert (trans)
    trans.props.direction = GstWebRTC.WebRTCRTPTransceiverDirection.SENDONLY

  receiver_entry.webrtcbin.connect ("on-negotiation-needed", on_negotiation_needed_cb, receiver_entry)
  receiver_entry.webrtcbin.connect("on-ice-candidate", on_ice_candidate_cb, receiver_entry)

  # Start processing
  receiver_entry.pipeline.set_state(Gst.State.PLAYING)
 
  return receiver_entry

def soup_websocket_message_cb (connection, data_type, message, receiver_entry):
  data_string = ""

  print ("websocket message received.....",message)

  if data_type == Soup.WebsocketDataType(2):
    print ("Error: Received unknown binary message")
    return
  elif data_type == Soup.WebsocketDataType(1):
    data = message.unref_to_data ()
    data_string = data.decode("utf-8")
    print("data from message",data_string)

  json_parser = Json.Parser ()
  if json_parser.load_from_data (data_string, -1) != True:
    print ("Unknown message")
    return

  root_json = json_parser.get_root ()
  #Fixme: if (%JSON_NODE_HOLDS_OBJECT (root_json)):
  if root_json == None:
    print ("Unknown message, failed")
    return

  root_json_object = root_json.get_object ();

  if root_json_object.has_member ("REPLY_CONNECTION_TYPE"):
    reply_type_string = root_json_object.get_string_member ("REPLY_CONNECTION_TYPE")

    if GLib.strcmp0 (reply_type_string, "parent") == 0:
      receiver_entry.is_parent = True
    else:
      receiver_entry.is_parent = False

    print ("Reply Type String = ", reply_type_string,
            " is_parent = ",receiver_entry.is_parent,
            " connection = ",receiver_entry.connection)

    receiver_entry = create_receiver_entry (receiver_entry)

    receiver_entry_table[connection] = receiver_entry
    return

  if root_json_object.has_member ("type") == False:
    print ("Error: Received message without type field")
    return
  type_string = root_json_object.get_string_member ("type")

  if root_json_object.has_member ("data") == False:
    print ("Error: Received message without data field")
    return
  data_json_object = root_json_object.get_object_member ("data")

  if GLib.strcmp0 (type_string, "sdp") == 0:

    if data_json_object.has_member ("type") == False:
      print ("Error:Received SDP message without type field")
      return
    sdp_type_string = data_json_object.get_string_member ("type")

    if GLib.strcmp0 (sdp_type_string, "answer") != 0:
      print("Expected SDP message type :answer: but got",sdp_type_string)
      return

    if data_json_object.has_member ("sdp") == False:
      print ("Error: Received SDP message without SDP string")
      return
    sdp_string = data_json_object.get_string_member ("sdp")

    print ("Received SDP: ", sdp_string)

    (ret,sdp) = GstSdp.sdp_message_new ()
    if ret != 0:
      print ("Failed: sdp_message_new failed")
      return

    #Fixme::::::::::::::::::: sdp_string supposed to be bytes???????!
    #ret = GstSdp.sdp_message_parse_buffer (data_json_object["sdp"],sdp)
    #if ret != 0:
    #  print ("Could not parse SDP string")
    #  return

    (res,sdp) = GstSdp.sdp_message_new_from_text (sdp_string)

    answer = GstWebRTC.WebRTCSessionDescription.new (GstWebRTC.WebRTCSDPType.ANSWER,sdp)
    if answer is None:
      print ("Error: Failed to Create answer")
      return

    promise = Gst.Promise.new ()
    receiver_entry.webrtcbin.emit ("set-remote-description", answer, promise)
    promise.interrupt ()

  elif GLib.strcmp0 (type_string, "ice") == 0:
    if data_json_object.has_member ("sdpMLineIndex") == False:
      print ("Error: Received ICE message without mline index\n")
      return
    mline_index = data_json_object.get_int_member ("sdpMLineIndex")

    if data_json_object.has_member ("candidate") == False:
      print ("Received ICE message without ICE candidate string\n")
      return
    candidate_string = data_json_object.get_string_member ("candidate")

    print ("Received ICE candidate with mline index", mline_index, "candidate: ", candidate_string)

    receiver_entry.webrtcbin.emit ("add-ice-candidate", mline_index, candidate_string)
  else:
    return

def soup_websocket_handler(server, connection, path, client_context,receiver_entry_table):
 
  print ("Processing web socket connection...",connection)

  connection.connect("closed", soup_websocket_closed_cb, receiver_entry_table)

  receiver_entry = ReceiverEntry (connection)
  receiver_entry_table[connection]=receiver_entry

  connection.connect("message", soup_websocket_message_cb, receiver_entry)

  send_command(connection, "REQUEST_CONNECTION_TYPE", None)

def hand_inter(signum, frame):
    sys.exit(1)

def main():
  Gst.init(None);
  global global_enable_facedetect

  signal.signal(signal.SIGINT, hand_inter)

  if not check_plugins():
    sys.exit(1)

  parser = argparse.ArgumentParser()
  parser.add_argument('--enable-facedetect', action='store_true', help='Enable facedetect')
  global_enable_facedetect = parser.parse_args().enable_facedetect

  print ("arg = ",global_enable_facedetect)
  soup_server = Soup.Server ()
  soup_server.add_websocket_handler ("/ws",None, None,
          soup_websocket_handler, receiver_entry_table)

  soup_server.listen_all (SOUP_HTTP_PORT, 0)

  print("WebRTC page link: http://127.0.0.1:",SOUP_HTTP_PORT);

  global_loop = GLib.MainLoop()
  try:
    global_loop.run()
  except:
    pass

  Gst.deinit ();

if __name__ == "__main__":
  main()
