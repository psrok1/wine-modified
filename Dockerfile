FROM i386/ubuntu
COPY . /opt/wine-source
WORKDIR /opt/wine-source
RUN BUILD_DEPS="build-essential gcc-multilib flex bison libxi-dev libfreetype6-dev libxcursor-dev libxrandr-dev libxcomposite-dev libtiff5-dev libxrender-dev libxml2-dev libxslt1-dev libjpeg-dev" && \
    apt update && apt install -y $BUILD_DEPS && \
    ./configure && make -j4 && DESTDIR=/opt/wine-dist make install && cd .. ; rm -rf wine-source ; \
    apt remove -y --purge $BUILD_DEPS && apt auto-remove -y \
