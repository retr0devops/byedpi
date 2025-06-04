TARGET = ciadpi

CPPFLAGS = -D_DEFAULT_SOURCE
CFLAGS += -I. -std=c99 -O2 -Wall -Wno-unused -Wextra -Wno-unused-parameter -pedantic
WIN_LDFLAGS = -lws2_32 -lmswsock

HEADERS = conev.h desync.h error.h extend.h kavl.h mpool.h packets.h params.h proxy.h win_service.h
SRC = packets.c main.c conev.c proxy.c desync.c mpool.c extend.c
WIN_SRC = win_service.c

OBJ = $(SRC:.c=.o)
WIN_OBJ = $(WIN_SRC:.c=.o)

LIB_OBJ = $(patsubst %.c,lib_%.o,$(SRC))
LIBTARGET = libciadpi.a

PREFIX := /usr/local
INSTALL_DIR := $(DESTDIR)$(PREFIX)/bin/

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) -o $(TARGET) $(OBJ) $(LDFLAGS)

windows: $(OBJ) $(WIN_OBJ)
	$(CC) -o $(TARGET).exe $(OBJ) $(WIN_OBJ) $(WIN_LDFLAGS)




lib: $(LIBTARGET)
	
$(LIBTARGET): $(LIB_OBJ)
	ar rcs $@ $(LIB_OBJ)
	
$(LIB_OBJ): $(HEADERS)
lib_%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -DCIADPI_LIB -c $< -o $@

$(OBJ): $(HEADERS)
.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $<

clean:
	rm -f $(TARGET) $(TARGET).exe $(OBJ) $(WIN_OBJ) $(LIB_OBJ) $(LIBTARGET)

install: $(TARGET)
	mkdir -p $(INSTALL_DIR)
	install -m 755 $(TARGET) $(INSTALL_DIR)
