SUMMARY = "IDT NTB userspace application channel kernel module"
DESCRIPTION = "A Linux NTB client driver for IDT NTB devices that exposes a character device for userspace VM-to-VM application traffic."
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://COPYING;md5=d1235e54ccbde07b307b638c79b854fe"

inherit module

SRC_URI = "file://Makefile \
           file://ntb-idt-app.c \
           file://COPYING"

S = "${WORKDIR}"

RPROVIDES:${PN} += "kernel-module-ntb-idt-app"