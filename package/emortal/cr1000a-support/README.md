# Verizon CR1000A support

The CR1000A device profile selects this package. It enables `KERNEL_DEVMEM`
and pulls in `cryptsetup`, `kmod-spi-dev`, the vendor helpers' libraries,
`ethtool`, ext4 and the storage crypto modules. With per-device root filesystems,
the package may appear as `m` in `.config`; the CR1000A image still includes it.
Kernel options are shared by all images built for that subtarget in one build.

Binary inputs are from [kinglove1990/files](https://github.com/kinglove1990/files/tree/62ebb91b9483bf712cc0aea315722289cbdc89fd),
commit `62ebb91b9483bf712cc0aea315722289cbdc89fd` (2025-12-09).
Their original checksums are in `vendor.sha256`. The source repository supplies
no license file for the binary payloads, so package metadata uses
`LicenseRef-Unknown`. The new packaging and service scripts are GPL-2.0-only.

At build time, `usrApp` is adjusted to use the selected libubus/libubox SONAMEs;
no system library is copied or overwritten at boot. These are prebuilt vendor
programs: checking their imported symbols does not prove all ABI or hardware
behavior, which still requires a CR1000A boot test.

The board-guarded `cr1000a-support` procd service starts three independent,
one-shot helpers at S99:

- RTL9303: discovers the SPI node through the existing `cisco,spi-petra` DT
  binding, creates the path expected by the vendor binary, initializes the
  switch, and exits its CLI after the original 30-second interval. A successful
  run is remembered until reboot. There is no persistent FIFO or `tail` process.
- Aquantia: applies the upstream WAN advertisement mask. The original firmware
  download command was commented out and remains opt-in; the download tool and
  firmware are included but are never run automatically.
- eMMC: locates `PARTNAME=data`, unlocks it with the provided key as
  `/dev/mapper/cr1000a-data`, and mounts it at `/mnt/data`. An absent partition
  is skipped; a failed mount closes a mapping opened by the helper. The key is
  installed with mode 0600. An existing mount is left alone.

The package does not replace `/etc/rc.local`. Remove any old manually installed
calls to `/lib/{rtl,aqr,mmc}/init.sh` from rc.local before migrating an existing
installation to avoid starting the helpers twice on the first boot.
