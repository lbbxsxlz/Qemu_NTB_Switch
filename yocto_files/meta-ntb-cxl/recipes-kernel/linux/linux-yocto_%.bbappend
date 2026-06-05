FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI += "file://fragment.cfg\
            file://0001-OUT-OF-SPECIFICATION-Inbound-MW.patch\
            file://0002-ntb_perf-report-throughput-in-KBytes-s.patch"
