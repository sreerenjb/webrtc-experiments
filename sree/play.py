import sys
import sys
sys.path.insert(0, "/home/sreerenj/opt/gstreamer/1.18/lib/python3.8/site-packages/gi/overrides/")

import gi


gi.require_version ('Gst', '1.0')
from gi.repository import GObject, Gst

def bus_call (bus, message, loop):
    t = message.type
    if t == Gst.MessageType.EOS:
        sys.stdout.write ("EOS \n")
        loop.quit()
    return True

def main(argv):
    if (len(argv)) < 2:
        sys.stderr.write ("Please provide a filename to play")
        sys.exit(1)
        return

    print (sys.path)
    uri = Gst.filename_to_uri(argv[1])
    print (uri)
    GObject.threads_init()
    Gst.init(None)

    '''move the next two lines above Gst.init to see the Gst.py path'''

    playbin = Gst.ElementFactory.make("playbin", None)
    if not playbin:
        sys.exit(1)

    playbin.set_property ("uri", uri)

    loop = GObject.MainLoop()

    bus = playbin.get_bus()
    bus.add_signal_watch()
    bus.connect("message",bus_call,loop)

    playbin.set_state(Gst.State.PLAYING)

    try:
        loop.run()
    except:
        pass

    playbin.set_state(Gst.State.NULL)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
