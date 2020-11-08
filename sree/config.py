"""globals"""

RTP_PAYLOAD_TYPE = "96"
RTP_CAPS_VP8 = "application/x-rtp,media=video,encoding-name=VP8,payload="
SOUP_HTTP_PORT = 57776
STUN_SERVER = "stun.l.google.com:19302"

# hash table to save the connections
receiver_entry_table = {}

SEND_SRC = """
videotestsrc is-live=true pattern="smpte" ! timeoverlay ! queue ! vp8enc ! rtpvp8pay ! queue ! capsfilter caps=application/x-rtp,media=video,payload=96,encoding-name=VP8
"""
SEND_UDP_SRC = """
udpsrc port=5600  !  queue ! capsfilter caps=application/x-rtp,media=video,payload=96,encoding-name=H264 !  queue
"""
