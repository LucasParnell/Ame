# Loaded by Switchroot U-Boot's distro script scanner. This firmware supports
# source/bootm, but its default environment also names unbuilt sysboot/booti
# paths. Use the legacy images and the firmware's staging addresses.
echo Ame: loading kernel, signed system bundle and device tree
setenv verify yes
setenv bootargs ame.boot=1
if load ${devtype} ${devnum}:${distro_bootpart} ${kernel_addr_r} /ame/boot/uImage; then
    if load ${devtype} ${devnum}:${distro_bootpart} ${ramdisk_addr_r} /ame/boot/system.uInitrd; then
        if load ${devtype} ${devnum}:${distro_bootpart} ${fdt_addr_r} /ame/l4t/ame-switch.dtb; then
            echo Ame: entering kernel
            bootm ${kernel_addr_r} ${ramdisk_addr_r} ${fdt_addr_r}
        fi
    fi
fi
echo Ame: boot failed before kernel handoff
exit 1
