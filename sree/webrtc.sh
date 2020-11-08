export WLD=$HOME/opt/libnice  # change this to another location if you prefer
export LD_LIBRARY_PATH=$WLD/lib:$LD_LIBRARY_PATH
export PKG_CONFIG_PATH=$WLD/lib/pkgconfig/:$WLD/share/pkgconfig/:$PKG_CONFIG_PATH
export PATH=$WLD/bin:$PATH

export GST_PLUGIN_PATH=/home/sreerenj/opt/libnice/lib/gstreamer-1.0/:$GST_PLUGIN_PATH
export GI_TYPELIB_PATH=/home/sreerenj/opt/gstreamer/1.18/lib/x86_64-linux-gnu/girepository-1.0/:/home/sreerenj/opt/libnice/lib/girepository-1.0/:$GI_TYPELIB_PATH

