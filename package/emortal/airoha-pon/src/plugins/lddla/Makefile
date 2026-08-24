KERNELDIR=$(KERNEL_DIR)

EXTRA_CFLAGS += -Werror
EXTRA_CFLAGS += -D CONFIG_DEBUG \


LDDLA_SRC	+= en7572_init.c en7572_i2c.c en7572_cmd.c 
LDDLA_OBJ	:= $(LDDLA_SRC:%.c=%.o)



en7572-objs 	:= $(LDDLA_OBJ)
obj-m		:= en7572.o

all:
	$(MAKE) -C $(KERNELDIR) M=`pwd` modules
	$(STRIP) --strip-unneeded en7572.ko
	
clean:
	$(MAKE) -C $(KERNELDIR) M=`pwd` clean
	
	