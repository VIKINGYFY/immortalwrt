DTS_DIR := $(DTS_DIR)/qcom

DEVICE_VARS += BOOT_SCRIPT

define Build/gl-ipq-factory-nand
	$(CP) $(BOOT_SCRIPT) $(KDIR_TMP)/
	$(TOPDIR)/scripts/mkits-qsdk-ipq-image.sh \
		$@.its \
		$(KDIR_TMP)/$(notdir $(BOOT_SCRIPT)) \
		ubi \
		$@
	PATH=$(LINUX_DIR)/scripts/dtc:$(PATH) mkimage -f $@.its $@.new
	@mv $@.new $@
	$(RM) $@.its $(KDIR_TMP)/$(notdir $(BOOT_SCRIPT))
endef

define Build/fit-inline-rootfs
	rm -f $@.dtb $@.kernel
	cp $@ $@.kernel
	cp $(word 2,$(1)) $@.dtb
	cp $@.kernel $@
	$(call Build/fit-its,$(word 1,$(1)) $@.dtb with-rootfs)
	$(call Build/fit-image,$(word 1,$(1)) $@.dtb with-rootfs)
	rootfs_offset="$$(grep -oba hsqs $@ | head -n1 | cut -d: -f1)"; \
	[ -n "$$rootfs_offset" ] || { echo "Failed to locate SquashFS in $@"; exit 1; }; \
	pad="$$(( (4096 - ($$rootfs_offset % 4096)) % 4096 ))"; \
	cp $(word 2,$(1)) $@.dtb; \
	dd if=/dev/zero bs=1 count="$$pad" >> $@.dtb 2>/dev/null; \
	cp $@.kernel $@; \
	$(call Build/fit-its,$(word 1,$(1)) $@.dtb with-rootfs)
	$(call Build/fit-image,$(word 1,$(1)) $@.dtb with-rootfs)
	rm -f $@.dtb $@.kernel
endef

define Device/ubnt_u7-pro-xgs
	DEVICE_VENDOR := Ubiquiti
	DEVICE_MODEL := UniFi 7
	DEVICE_VARIANT := Pro XGS
	# Stock U-Boot probes config-a6a4 on this board.
	DEVICE_DTS_CONFIG := config-a6a4
	SOC := ipq5332
	SUPPORTED_DEVICES += ubnt,u7-pro-xgs
	DEVICE_PACKAGES := ipq-wifi-ubnt_u7-pro-xgs ath12k-firmware-qcn9274-ddwrt \
		kmod-phy-realtek rtl826x-firmware
	KERNEL := kernel-bin | lzma
	KERNEL_INITRAMFS := kernel-bin | lzma | \
		fit lzma $$(KDIR)/image-$$(firstword $$(DEVICE_DTS)).dtb with-initrd | pad-to 64k
	KERNEL_INITRAMFS_SUFFIX := .itb
	IMAGE_SIZE := 128m
	IMAGES := sysupgrade.itb
	IMAGE/sysupgrade.itb := append-kernel | \
		fit-inline-rootfs lzma $$(KDIR)/image-$$(firstword $$(DEVICE_DTS)).dtb | \
		check-size | append-metadata
endef
TARGET_DEVICES += ubnt_u7-pro-xgs

define Device/glinet_gl-be6500
	$(call Device/FitImage)
	$(call Device/UbiFit)
	DEVICE_VENDOR := GL.iNet
	DEVICE_MODEL := GL-BE6500
	DEVICE_DTS_CONFIG := config@mi01.2
	SOC := ipq5332
	SUPPORTED_DEVICES += gl.inet,gl-be6500
	BLOCKSIZE := 256k
	PAGESIZE := 4096
	KERNEL_INSTALL := 1
	KERNEL_SIZE := 6096k
	IMAGE_SIZE := 25344k
	BOOT_SCRIPT := glinet_gl-be6500.bootscript
	IMAGES += factory.bin
	IMAGE/factory.bin := append-ubi | gl-ipq-factory-nand
	DEVICE_PACKAGES := ipq-wifi-glinet_gl-be6500 ath12k-firmware-qcn9274-ddwrt \
		kmod-hwmon-pwmfan kmod-rtl837x-dsa
endef
TARGET_DEVICES += glinet_gl-be6500

define Device/xiaomi_be3600-pro-wired-common
	$(call Device/FitImage)
	$(call Device/UbiFit)
	DEVICE_VENDOR := Xiaomi
	DEVICE_MODEL := BE3600 Pro
	DEVICE_DTS_CONFIG := config@mi04.1-c2
	BLOCKSIZE := 128k
	PAGESIZE := 2048
	SOC := ipq5332
	DEVICE_PACKAGES := -kmod-leds-gpio kmod-rtl837x-dsa
endef

define Device/xiaomi_be3600-pro-wired-p5
	$(call Device/xiaomi_be3600-pro-wired-common)
	DEVICE_VARIANT := p5
	SUPPORTED_DEVICES += xiaomi,be3600-pro-wired-p5
endef
TARGET_DEVICES += xiaomi_be3600-pro-wired-p5

define Device/xiaomi_be3600-pro-wired-p8
	$(call Device/xiaomi_be3600-pro-wired-common)
	DEVICE_VARIANT := p8
	SUPPORTED_DEVICES += xiaomi,be3600-pro-wired-p8
endef
TARGET_DEVICES += xiaomi_be3600-pro-wired-p8
