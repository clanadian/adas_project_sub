FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# 파일명을 원본과 다르게 둔다(interfaces-static) - 원본 recipe도
# "file://interfaces"를 쓰므로 같은 이름이면 어느 쪽이 fetch되는지
# 애매해진다(2026-08-20, 실제로 원본이 설치되는 걸 겪음). do_install
# 마지막에 이 파일로 강제 덮어쓴다.
SRC_URI += "file://interfaces-static"

do_install:append() {
    install -m 0644 ${WORKDIR}/interfaces-static ${D}${sysconfdir}/network/interfaces
}
