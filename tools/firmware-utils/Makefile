#
# Copyright (C) 2006-2020 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#
include $(TOPDIR)/rules.mk

PKG_NAME:=firmware-utils
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL=$(PROJECT_GIT)/project/firmware-utils.git
PKG_SOURCE_DATE:=2026-11-16
PKG_SOURCE_VERSION:=dcf2ab2612c599dee3df2fd1bdc537eb65a96127
PKG_MIRROR_HASH:=6069b23db9a4715afecd26279dba3680137ea4d21e58343d3c83aa0f85a67605

include $(INCLUDE_DIR)/host-build.mk
include $(INCLUDE_DIR)/cmake.mk

$(eval $(call HostBuild))
