"c:\Program Files\qemu\qemu-system-i386" -drive file=images\os-fd.img,if=floppy,media=disk,format=raw -drive file=images\os-hd.img,media=disk,format=raw -boot menu=off -serial mon:stdio -m 2 -vga std -net nic,model=ne2k_isa -net tap,ifname=tap -monitor vc
