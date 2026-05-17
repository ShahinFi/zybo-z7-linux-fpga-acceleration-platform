#
# This file is the zybo-accel-reg-test recipe.
#

SUMMARY = "Register regression application for zybo_accel"
SECTION = "PETALINUX/apps"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://zybo_accel_reg_test.c \
	   file://Makefile \
	file://zybo_accel_uapi.h \
		  "

S = "${WORKDIR}"

do_compile() {
	     oe_runmake
}

do_install() {
	     install -d ${D}${bindir}
	     install -m 0755 zybo-accel-reg-test ${D}${bindir}
}
