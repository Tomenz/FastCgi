
# ACHTUNG unbedingt TABS benutzen beim einrücken

CC = g++
#CFLAGS = -ggdb -w -pthread
CFLAGS = -Wall -O3 -std=c++14 -pthread -ffunction-sections -fdata-sections
INC_PATH = -I ..
TARGET = libfastcgi.a

OBJ = $(patsubst %.cpp,%.o,$(wildcard *.cpp))

$(TARGET): $(OBJ)
	ar rs $@ $^

%.o: %.cpp %.h
	$(CC) $(CFLAGS) $(INC_PATH) -c $<

clean:
	rm -f $(TARGET) $(OBJ) *~

