SUMMARY = "IDT NTB SISCI-like segment mapping kernel module"
DESCRIPTION = "A Linux NTB client driver that exposes a SISCI-like segment/connect/mmap interface for userspace remote-memory experiments."
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://COPYING;md5=c94f9406457821714facaa0f1b2a6972"

inherit module

SRC_URI = "file://Makefile \
           file://ntb-idt-sisci-app.c \
           file://COPYING"

S = "${WORKDIR}"

RPROVIDES:${PN} += "kernel-module-ntb-idt-sisci-app"
