FROM ubuntu:18.04 as base

RUN apt-get update \
    &&  DEBIAN_FRONTEND=noninteractive apt-get -y --no-install-recommends install \
    neovim \
    wget \
    build-essential \
    python3 \
    python3-pip \
    git \
    cmake \
    autoconf \
    automake \
    libtool \
    gstreamer-1.0 \
    gstreamer1.0-dev \
    libgstreamer1.0-0 \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    gstreamer1.0-doc \
    gstreamer1.0-tools \
    gstreamer1.0-x \
    gstreamer1.0-alsa \
    gstreamer1.0-gl \
    gstreamer1.0-gtk3 \
    gstreamer1.0-qt5 \
    gstreamer1.0-pulseaudio \
    python-gst-1.0 \
    libgirepository1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libcairo2-dev \
    gir1.2-gstreamer-1.0 \
    python3-gi \
    python-gi-dev \
    && rm -rf /var/lib/apt/lists/*

# TODO(unrahul): cleaup
RUN apt-get update \
    && apt-get install -y python3-dev \
    && rm -rf /var/lib/apt/lists/*
RUN apt-get update \
    && apt-get install -y\
    build-essential libssl-dev libffi-dev \
    libxml2-dev libxslt1-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
RUN pip3 install --no-cache-dir setuptools wheel scikit-build \
    && pip3 install --no-cache-dir \
    requests aiohttp aiofiles sanic \
    flask quart uvicorn fastapi

RUN pip3 install --no-cache-dir \
    torch torchvision \
    -f https://download.pytorch.org/whl/torch_stable.html

RUN pip3 install --no-cache-dir \
    jupyter bpython matplotlib plotnine seaborn \
    scikit-learn pandas pillow numpy ipython \
    pytorch-lightning aiohttp aiofiles opencv-python \
    kornia

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y \
    python3-numpy python3-cffi python3-aiohttp \
    libavformat-dev libavcodec-dev libavdevice-dev libavutil-dev \
    libswscale-dev libswresample-dev libavfilter-dev libopus-dev \
    libvpx-dev pkg-config libsrtp2-dev python3-opencv pulseaudio \
    && dpkg-reconfigure --frontend noninteractive tzdata

# ffmpeg and srtp
RUN apt-get update \
    && apt-get install -y \
    software-properties-common \
    && add-apt-repository -y ppa:freedomrobotics/ppa \
    && apt-get update \
    && apt-get install -y ffmpeg4.2  libsrtp2-dev


RUN apt-get install -y libsrtp2-1
ENV PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
RUN apt-get install -y libavdevice-dev libavfilter-dev libopus-dev libvpx-dev pkg-config
RUN pip3 install pip --upgrade \
    && pip3 install --no-cache-dir aiortc

FROM ubuntu:18.04

COPY --from=base / /
WORKDIR /workspace
RUN bash -c "$(wget https://raw.githubusercontent.com/ohmybash/oh-my-bash/master/tools/install.sh -O -)"

CMD bash
