# Requer PS2SDK (ps2dev) + gsKit + ps2sdk-ports (libpng, zlib)
# Compilar:  make   ->  PS5LAUNCHER.ELF

IRX_LIST = iomanX fileXio bdm bdmfs_fatfs usbd usbmass_bd

EE_BIN  = PS5LAUNCHER.ELF
EE_SRC_DIR = src/
EE_OBJS = main.o config.o $(IRX_LIST:%=%_irx.o)

GSKIT ?= $(PS2DEV)/gsKit

EE_INCS += -I$(GSKIT)/include -I$(PS2SDK)/ports/include -Isrc
EE_LDFLAGS += -L$(GSKIT)/lib -L$(PS2SDK)/ports/lib
EE_LIBS = -lgskit -ldmakit -lgskit_toolkit -lpng -lz -lpad -lpatches \
          -lelf-loader -lkernel -lc -lm

all: $(EE_BIN)

%_irx.c:
	$(PS2SDK)/bin/bin2c $(PS2SDK)/iop/irx/$*.irx $@ $*_irx

clean:
	rm -f $(EE_BIN) *.o *_irx.c

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal