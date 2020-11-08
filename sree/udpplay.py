#Author : Sreerenj Balachandran <bsreerenj@gmail.com>

import os
import sys
import signal

import gi
from gi.repository import GObject
from gi.repository import GLib

gi.require_version('Gst', '1.0')
from gi.repository import Gst

SEND_SRC='''
videotestsrc is-live=true pattern=smpte ! timeoverlay ! queue ! vp8enc ! rtpvp8pay ! queue ! application/x-rtp,media=video,payload=96,encoding-name=VP8 ! queue ! rtpvp8depay ! vp8dec ! videoconvert ! xvimagesink
'''

def main():
  Gst.init(None)

  pipeline = Gst.parse_bin_from_description (SEND_SRC, True)
  pipeline.set_state(Gst.State.PLAYING)
  loop = GLib.MainLoop()
  try:
    loop.run()
  except:
    pass

  print ("stopping")
  pipeline.set_state(Gst.State.NULL)

  Gst.deinit ()
  return

if __name__ == "__main__":
  main()
