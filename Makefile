
# ACHTUNG unbedingt TABS benutzen beim einrücken

CC = g++
ifeq ($(DEBUG), yes)
CFLAGS = -ggdb -pthread
else
CFLAGS = -Wall -O3 -pthread -ffunction-sections -fdata-sections
endif
INC_PATH = -I ..
TARGET = libfastcgi.a

OBJ = $(patsubst %.cpp,%.o,$(wildcard *.cpp))

$(TARGET): $(OBJ)
	ar rs $@ $^

%.o: %.cpp %.h
	$(CC) $(CFLAGS) $(INC_PATH) -c $<

clean:
	rm -f $(TARGET) $(OBJ) *~

