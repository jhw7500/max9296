# Out-of-tree build of mainline dw100 against NXP BSP 5.10.35 (spike)
KERNEL_SRC ?= /opt/desktop/build-desktop/tmp/work/imx8mpevk-fsl-linux/linux-imx/5.10.35+git999-r0/linux-imx-5.10.35+git999

obj-m += dw100.o

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean
