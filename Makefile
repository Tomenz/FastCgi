
# ACHTUNG unbedingt TABS benutzen beim einrücken

CC = g++
#CFLAGS = -ggdb -w -m32 -D _DEBUG -D ZLIB_CONST -pthread
CFLAGS = -Wall -O3 -std=c++14 -pthread -ffunction-sections -fdata-sections
TARGET = libfastcgi.a


OBJ = $(patsubst %.cpp,%.o,$(wildcard *.cpp))

$(TARGET): $(OBJ)
	ar rs $@ $^

%.o: %.cpp %.h
	$(CC) $(CFLAGS) $(INC_PATH) -c $<

clean:
	rm -f $(TARGET) $(OBJ) *~

