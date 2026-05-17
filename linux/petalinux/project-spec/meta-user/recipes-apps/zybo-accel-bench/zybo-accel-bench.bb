SUMMARY = "End-to-end accelerator-path benchmark for zybo_accel"
SECTION = "PETALINUX/apps"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://zybo_accel_bench.c \
           file://zybo_accel_uapi.h \
"

S = "${WORKDIR}"

do_compile() {
        ${CC} ${CFLAGS} ${LDFLAGS} \
                -I${S} \
                -o zybo-accel-bench \
                zybo_accel_bench.c
}

do_install() {
        install -d ${D}${bindir}
        install -m 0755 zybo-accel-bench ${D}${bindir}
}
