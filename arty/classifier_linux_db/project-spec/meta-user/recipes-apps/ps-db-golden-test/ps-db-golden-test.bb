SUMMARY = "DB classifier board golden verification tool"
LICENSE = "CLOSED"

inherit cmake externalsrc

EXTERNALSRC = "${THISDIR}/../../../../../ps_db"
EXTERNALSRC_BUILD = "${WORKDIR}/build"
OECMAKE_TARGET_COMPILE = "ps_db_golden_test"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/ps_db_golden_test ${D}${bindir}/ps_db_golden_test
}
