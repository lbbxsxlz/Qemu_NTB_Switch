ARG BASE_IMAGE=ubuntu:22.04
FROM ${BASE_IMAGE}

ARG http_proxy
ARG https_proxy
ARG no_proxy
ARG HTTP_PROXY
ARG HTTPS_PROXY
ARG NO_PROXY

# Install deps
RUN set -eux; \
	apt_proxy="${http_proxy:-${HTTP_PROXY:-}}"; \
	apt_secure_proxy="${https_proxy:-${HTTPS_PROXY:-$apt_proxy}}"; \
	proxy_config=/etc/apt/apt.conf.d/99proxy; \
	: > "$proxy_config"; \
	if [ -n "$apt_proxy" ]; then \
		printf 'Acquire::http::Proxy "%s";\n' "$apt_proxy" >> "$proxy_config"; \
	fi; \
	if [ -n "$apt_secure_proxy" ]; then \
		printf 'Acquire::https::Proxy "%s";\n' "$apt_secure_proxy" >> "$proxy_config"; \
	fi; \
	if [ ! -s "$proxy_config" ]; then \
		rm -f "$proxy_config"; \
	fi; \
	apt-get update; \
	if ! apt-cache show file >/dev/null 2>&1; then \
		echo "APT metadata is unavailable. Pass proxy settings via docker build --build-arg http_proxy=... --build-arg https_proxy=... or use run_container.sh, which forwards them automatically." >&2; \
		exit 1; \
	fi; \
	DEBIAN_FRONTEND=noninteractive apt-get -y install \
		ca-certificates \
		chrpath \
		cpio \
		diffstat \
		file \
		gawk \
		git \
		liblz4-tool \
		locales \
		python3 \
		python3-pip \
		wget \
		zstd; \
	rm -rf /var/lib/apt/lists/*

# Set the locale
RUN sed -i '/en_US.UTF-8/s/^# //g' /etc/locale.gen && locale-gen
ENV LANG=en_US.UTF-8
ENV LC_ALL=en_US.UTF-8

# ID for new user
ARG user_id=1000

ENV BUILD_DIRNAME=build_dir
ENV PROJECT_PATH=/home/user/project
ENV BUILD_PATH=$PROJECT_PATH/$BUILD_DIRNAME

# Create user for yocto
RUN useradd -rm -d /home/user -s /bin/bash -g root -G sudo user -u $user_id
USER user

# Creating build path
RUN mkdir -p $BUILD_PATH
WORKDIR $PROJECT_PATH

# Copy configs and scripts
ADD ./scripts $PROJECT_PATH/scripts 
ADD ./yocto_files $PROJECT_PATH/yocto_files
ADD ./user_src $PROJECT_PATH/user_src
ADD ./kernel_src $PROJECT_PATH/kernel_src

ENTRYPOINT ["/home/user/project/scripts/dispatch_docker_command.sh"]

CMD ["--command=build"]
