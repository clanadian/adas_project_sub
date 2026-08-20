SUMMARY = "DB classifier board TCP server"
LICENSE = "CLOSED"

inherit cmake externalsrc

EXTERNALSRC = "${THISDIR}/../../../../../ps_db"
EXTERNALSRC_BUILD = "${WORKDIR}/build"
OECMAKE_TARGET_COMPILE = "ps_classifier_server"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/ps_classifier_server ${D}${bindir}/ps_classifier_server
}
