SUMMARY = "Recipe for  build an external adas-classifier Linux kernel module"
SECTION = "PETALINUX/modules"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://COPYING;md5=12f884d2ae1ff87c09e5b7ccc2c4ca7e"

inherit module

INHIBIT_PACKAGE_STRIP = "1"

SRC_URI = "file://Makefile \
           file://adas-classifier.c \
	   file://adas_classifier_uapi.h \
	   file://adas_classifier_eb_hw.h \
	   file://COPYING \
          "

KERNEL_MODULE_AUTOLOAD += "adas-classifier"

S = "${WORKDIR}"

# The inherit of module.bbclass will automatically name module packages with
# "kernel-module-" prefix as required by the oe-core build environment.
