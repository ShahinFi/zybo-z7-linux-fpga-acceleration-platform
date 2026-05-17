SUMMARY = "DMA XOR validation application for zybo_accel"
SECTION = "PETALINUX/apps"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://zybo_accel_dma_test.c \
           file://zybo_accel_uapi.h \
"

S = "${WORKDIR}"

do_compile() {
	${CC} ${CFLAGS} ${LDFLAGS} \
		-I${S} \
		-o zybo-accel-dma-test \
		zybo_accel_dma_test.c
}

do_install() {
	install -d ${D}${bindir}
	install -m 0755 zybo-accel-dma-test ${D}${bindir}
}
