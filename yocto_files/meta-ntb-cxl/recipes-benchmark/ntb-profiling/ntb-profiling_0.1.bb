SUMMARY = "User-space NTB bandwidth profiling tool"
DESCRIPTION = "Builds ntb_profiling, a unified bandwidth tester for generic /dev/ntb_idt_app and SISCI-like /dev/ntb_idt_sisci backends."
LICENSE = "CLOSED"

SRC_URI = "file://ntb_profiling.c \
           file://ntb_generic.c \
           file://ntb_utils.c \
           file://ntb_profiling.h \
           file://ntb_utils.h \
           file://ntb_sisci.c \
           file://ntb_sisci.h"

S = "${WORKDIR}"
B = "${WORKDIR}"

do_compile() {
    ${CC} ${CFLAGS} ${CPPFLAGS} ${S}/ntb_profiling.c ${S}/ntb_generic.c ${S}/ntb_utils.c ${S}/ntb_sisci.c -o ${B}/ntb_profiling ${LDFLAGS}
}

do_install() {
	install -d ${D}${bindir}
	install -m 0755 ${B}/ntb_profiling ${D}${bindir}/ntb_profiling
}

RRECOMMENDS:${PN} += "ntb-idt-app ntb-idt-sisci-app"
