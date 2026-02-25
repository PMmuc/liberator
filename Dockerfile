FROM ubuntu:24.04 AS libfuzzpp_dev_image


RUN --mount=type=cache,target=/var/cache/apt apt-get -q update && \
    DEBIAN_FRONTEND="noninteractive" \
    apt-get -y install --no-install-suggests --no-install-recommends \
    sudo make ninja-build texinfo bison zsh ccache autoconf libtool \
    zlib1g-dev liblzma-dev libjpeg-turbo8-dev automake cmake nasm \
    build-essential git openssh-client python3 python3-dev \
    python3-setuptools python-is-python3 python3-venv python3-pip \
    libtool libtool-bin libglib2.0-dev wget vim jupp nano \
    bash-completion less apt-utils apt-transport-https curl  \
    ca-certificates gnupg dialog libpixman-1-dev gnuplot-nox \
    nodejs npm graphviz libtinfo-dev libz-dev zip unzip libedit2 \
    tmux tree gdb jq bc cloc ccache lsb-release lsof cargo rsync \
    && rm -rf /var/lib/apt/lists/*


RUN sudo apt-get update && \
    sudo apt-get install -y \
    zlib1g-dev \
    unzip \
    cmake \
    gcc \
    g++ \
    nodejs \
    libedit2 \
    zlib1g \
    libzstd1 \
    libncursesw6 \
    libxml2 \
    libicu74 && \
    sudo ln -s /usr/lib/x86_64-linux-gnu/libxml2.so.2 /usr/lib/x86_64-linux-gnu/libxml2.so.16 && \
    sudo ln -s /usr/lib/x86_64-linux-gnu/libedit.so.2 /usr/lib/x86_64-linux-gnu/libedit.so.0 && \
    sudo ln -s /usr/lib/x86_64-linux-gnu/libicuuc.so.74 /usr/lib/x86_64-linux-gnu/libicuuc.so.76 && \
    sudo ln -s /usr/lib/x86_64-linux-gnu/libicudata.so.74 /usr/lib/x86_64-linux-gnu/libicudata.so.76 && \
    sudo apt-get clean && \
    sudo rm -rf /var/lib/apt/lists/*^

RUN wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | sudo apt-key add - && \
    echo "deb http://apt.llvm.org/focal/ llvm-toolchain-focal-16 main" | sudo tee /etc/apt/sources.list.d/llvm.list

# Clang dependencies
RUN --mount=type=cache,target=/var/cache/apt apt-get update && apt-get full-upgrade -y && \
    DEBIAN_FRONTEND="noninteractive" \
    apt-get -y install --no-install-suggests --no-install-recommends \
    gcc g++ libncurses6 clang-16 llvm-16-dev libclang-16-dev


ARG USERNAME=libfuzz
ARG USER_UID=1000
ARG USER_GID=$USER_UID

RUN ls -la /usr/lib/x86_64-linux-gnu/cmake/z3/ || echo "Directory doesn't exist"

RUN (userdel -r $(getent passwd 1000 | cut -d: -f1) 2>/dev/null || true) \
    && (groupdel $(getent group 1000 | cut -d: -f1) 2>/dev/null || true)

RUN groupadd --gid $USER_GID $USERNAME \
    && useradd --uid $USER_UID --gid $USER_GID -m -s/bin/zsh $USERNAME \
    # [Optional] Add sudo support. Omit if you don't need to install software after connecting.
    && apt-get update \
    && apt-get install -y sudo \
    && echo $USERNAME ALL=\(root\) NOPASSWD:ALL > /etc/sudoers.d/$USERNAME \
    && chmod 0440 /etc/sudoers.d/$USERNAME \
    && chown -R root /usr/bin/sudo \
    && chmod 4755 /usr/bin/sudo \
    && chown -R $USERNAME /home/$USERNAME

ENV HOME=/home/${USERNAME}
USER ${USERNAME}
WORKDIR ${HOME}
ENV CCACHE_DIR=${HOME}/.ccache
RUN echo "export PATH=\$PATH:${HOME}/.local/bin" >> ~/.bashrc
RUN echo "export PATH=\$PATH:${HOME}/.local/bin" >> ~/.zshrc
RUN --mount=type=cache,target=${CCACHE_DIR} mkdir -p ${CCACHE_DIR} && sudo -E chown -R ${USERNAME}:${USERNAME} ${CCACHE_DIR}

RUN --mount=type=cache,target=${HOME}/.ccache/ git clone https://github.com/facebook/zstd.git && \
    cd zstd/build/cmake && \
    mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -G Ninja \
    .. && \
    sudo ninja install

# install z3
RUN --mount=type=cache,target=${HOME}/.ccache/ git clone https://github.com/Z3Prover/z3.git && \
    cd z3 && \
    git checkout z3-4.8.12 && \
    mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -G Ninja \
    .. && \
    sudo ninja install

RUN pip3 install --break-system-packages ipython
RUN sh -c "$(curl -fsSL https://raw.github.com/ohmyzsh/ohmyzsh/master/tools/install.sh)"

RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
RUN $HOME/.cargo/bin/cargo install casr

ENV LIBFUZZ=/workspaces/libfuzz

RUN --mount=type=cache,target=/var/cache/apt sudo apt-get update && sudo apt-get full-upgrade -y && \
    DEBIAN_FRONTEND="noninteractive" \
    sudo apt-get -y install --no-install-suggests --no-install-recommends \
    gcc g++ libncurses6  clang-16 llvm-16-dev

# gef
RUN echo "export LC_CTYPE=C.UTF-8" >> ~/.bashrc
RUN echo "export LC_CTYPE=C.UTF-8" >> ~/.zshrc
RUN bash -c "$(curl -fsSL https://gef.blah.cat/sh)"

COPY ./requirements.txt ${HOME}/python/requirements.txt
RUN cd ${HOME}/python && python3 -m pip install --break-system-packages -r requirements.txt

# Copy local build of LLVM-16
COPY --chown=${USERNAME}:${USERNAME} ./llvm-16/ ${HOME}/llvm-16

RUN --mount=type=bind,source=./llvm-16,target=/tmp/llvm-16 \
    mkdir -p ${HOME}/llvm-16 && \
    cp -r /tmp/llvm-16/. ${HOME}/llvm-16/


ENV SVF_VERSION="3.2"
ENV LLVM_DIR="/home/libfuzz/llvm-16/"

# SVF
# checkout and build SVF 3.2
RUN --mount=type=cache,target=${HOME}/.ccache/ git clone https://github.com/SVF-tools/SVF.git && \
    cd SVF && \
    git checkout "tags/SVF-${SVF_VERSION}"&& \
    mkdir -p build && cd build && \
    cmake -G Ninja -DSVF_WARN_AS_ERROR=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSVF_ENABLE_ASSERTIONS=ON -DCMAKE_INSTALL_PREFIX=/home/libfuzz/SVF-${SVF_VERSION} -DSVF_ENABLE_RTTI=ON .. && \
    ninja install

# ------------------------------------------------------------------------------------------------------------------
# TARGET FOR LIBRARY DEBUG
FROM libfuzzpp_dev_image AS libfuzzpp_debug

ENV TOOLS_DIR ${HOME}
ARG target_name=c-ares
ARG USERNAME=libfuzz
ARG USER_UID=1000
ARG USER_GID=1000

ENV TARGET_NAME=${target_name}

COPY --link ./llvm-project/build ${HOME}/LLVM/
ENV LLVM_DIR=${HOME}/llvm-16
ENV SVF_DIR=${HOME}/SVF-3.2
# ENV LLVM_DIR ${LIBFUZZ}/llvm-project/build
ENV TARGET=${HOME}/library
ENV DRIVER_FOLDER=${LIBFUZZ}/workdir/${TARGET_NAME}/drivers
ENV DRIVER="*"

RUN mkdir -p ${HOME}/${TARGET_NAME}
WORKDIR ${HOME}/${TARGET_NAME}
RUN ./analysis.sh ${TARGET_NAME} --fetch-only
COPY --chown=${USERNAME}:${USERNAME}  ./targets/${TARGET_NAME}/build_library.sh ${HOME}/${TARGET_NAME}
RUN sed -i 's/-g /-gdwarf-4 /g' ./build_library.sh;
RUN sed -i 's/-g\"/-gdwarf-4\"/g' ./build_library.sh
RUN ./build_library.sh
COPY --chown=${USERNAME}:${USERNAME}  ./targets/${TARGET_NAME}/compile_driver.sh ${HOME}/${TARGET_NAME}
RUN sed -i 's/-g /-gdwarf-4 /g' ./compile_driver.sh
RUN bash -c "$(curl -fsSL https://gef.blah.cat/sh)"
RUN echo "PROMPT=\"Debug \"\$PROMPT" >> ~/.zshrc

WORKDIR ${LIBFUZZ}

# ------------------------------------------------------------------------------------------------------------------
# TARGET FOR LIBRARY ANALYSIS
FROM libfuzzpp_dev_image AS libfuzzpp_analysis

# 1. Build condition_extractor
# 2. Run analysis.sh
ARG USERNAME=libfuzz
ARG USER_UID=1000
ARG USER_GID=1000

ENV TOOLS_DIR=${HOME}

RUN mkdir -p ${TOOLS_DIR}/condition_extractor/
RUN mkdir -p ${TOOLS_DIR}/tool/misc/
RUN sudo apt-get install zlib1g-dev unzip cmake gcc g++ libtinfo6 nodejs
COPY --chown=${USERNAME}:${USERNAME} ./condition_extractor ${TOOLS_DIR}/condition_extractor/
COPY --chown=${USERNAME}:${USERNAME} ./tool/misc/extract_included_functions.py ${TOOLS_DIR}/tool/misc/
# ENV SVF_DIR /home/libfuzz/SVF
# ENV LLVM_DIR /home/libfuzz/SVF
RUN cd ${TOOLS_DIR}/condition_extractor && rm -Rf CMakeCache.txt CMakeFiles && ./bootstrap.sh && make -j

# NOTE: start_analysis.sh finds out its configuration automatically

#COPY LLVM/update-alternatives-clang.sh .
#RUN sudo ./update-alternatives-clang.sh 12 200
ENV PATH="/home/libfuzz/llvm-16/bin:${PATH}"
ENV LD_LIBRARY_PATH="/home/libfuzz/llvm-16/lib:${LD_LIBRARY_PATH}"
ENV PATH $PATH:${HOME}/.local/bin
RUN echo $PATH
CMD ${LIBFUZZ}/targets/start_analysis.sh

# ------------------------------------------------------------------------------------------------------------------
# TARGET FOR DRIVER GENERATION
FROM libfuzzpp_dev_image AS libfuzzpp_drivergeneration

ARG target_name=c-ares

# NOTE: start_generate_drivers.sh finds out its configuration automatically
WORKDIR ${LIBFUZZ}/targets/${TARGET_NAME}
CMD ${LIBFUZZ}/targets/start_generate_drivers.sh

# ------------------------------------------------------------------------------------------------------------------
# TARGET FOR FUZZING SESSION
FROM libfuzzpp_dev_image AS libfuzzpp_fuzzing

ARG USERNAME=libfuzz
ARG USER_UID=1000
ARG USER_GID=1000
ARG target_name=c-ares
# ARG timeout=10m
# ARG driver=*.cc

# COPY LLVM/update-alternatives-clang.sh .
# RUN sudo ./update-alternatives-clang.sh 12 200
# ENV LLVM_DIR /usr
COPY --link ./llvm-project/build ${HOME}/LLVM/
ENV LLVM_DIR=${HOME}/LLVM

ENV TARGET_NAME=${target_name}
ENV TARGET=${HOME}/library
ENV DRIVER_FOLDER=${LIBFUZZ}/workdir/${TARGET_NAME}/drivers

# I want to install the library at building time, so later I only need to build
# the drivers
WORKDIR ${LIBFUZZ}/targets/${TARGET_NAME}
RUN sudo mkdir -p ${TARGET} && sudo chown -R ${USERNAME}:${USERNAME} ${TARGET}
COPY --chown=${USERNAME}:${USERNAME}  ./analysis.sh ${LIBFUZZ}/
COPY --chown=${USERNAME}:${USERNAME}  ./targets/targets.txt ${LIBFUZZ}/targets/
COPY --chown=${USERNAME}:${USERNAME}  ./targets/${TARGET_NAME}/config.sh ${LIBFUZZ}/targets/${TARGET_NAME}/
RUN ./analysis.sh ${TARGET_NAME} --fetch-only
COPY --chown=${USERNAME}:${USERNAME}  ./targets/${TARGET_NAME}/build_library.sh ${LIBFUZZ}/targets/${TARGET_NAME}
RUN ./build_library.sh

# NOTE: start_fuzz_driver.sh finds out its configuration automatically
WORKDIR /tmp
CMD ${LIBFUZZ}/targets/start_fuzz_driver.sh

# ------------------------------------------------------------------------------------------------------------------
# TARGET FOR COVERAGE
FROM libfuzzpp_fuzzing AS libfuzzpp_coverage

WORKDIR ${LIBFUZZ}
ENV PROJECT_COVERAGE ${LIBFUZZ}/workdir/${TARGET_NAME}/coverage_data
ENV DRIVER_FOLDER ${LIBFUZZ}/workdir/${TARGET_NAME}/drivers
ENV CORPUS_FOLDER ${LIBFUZZ}/workdir/${TARGET_NAME}/corpus_new
CMD ${LIBFUZZ}/targets/start_coverage.sh

# ------------------------------------------------------------------------------------------------------------------
# TARGET FOR CRASH CLUSTERING
FROM libfuzzpp_fuzzing AS libfuzzpp_crash_cluster

WORKDIR ${LIBFUZZ}
ENV TARGET_WORKDIR ${LIBFUZZ}/workdir/${TARGET_NAME}
CMD ${LIBFUZZ}/targets/start_clustering.sh

# ------------------------------------------------------------------------------------------------------------------
# TARGET FOR FUZZING CAMPAIGNS
FROM libfuzzpp_dev_image AS libfuzzpp_fuzzing_campaigns
COPY LLVM/update-alternatives-clang.sh .
RUN sudo ./update-alternatives-clang.sh 12 200
WORKDIR ${LIBFUZZ}

# ------------------------------------------------------------------------------------------------------------------
# TARGET FOR DYNAMIC DRIVER CREATION
FROM libfuzzpp_fuzzing AS libfuzzpp_dyndrvgen
ENV DRIVER_FOLDER=""
WORKDIR ${LIBFUZZ}
COPY LLVM/update-alternatives-clang.sh .
RUN sudo ./update-alternatives-clang.sh 16 200
CMD ${LIBFUZZ}/targets/start_dyndrvcreation.sh
