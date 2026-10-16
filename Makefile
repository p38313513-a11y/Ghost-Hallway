TARGET = GhostHallway
OBJS = main.o

CFLAGS = -O2 -G0 -Wall -std=gnu99
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

BUILD_PRX = 1
PSP_FW_VERSION = 371
PSP_EBOOT_TITLE = Ghost Hallway
EXTRA_TARGETS = EBOOT.PBP

LIBS = -lpspdebug -lpspdisplay -lpspctrl -lpspkernel

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
