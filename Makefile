CC = gcc
CFLAGS = -g -Wall #-Werror

SRC = fs-sim.c
OBJ = $(SRC:.c=.o)

TARGET = fs 

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET)

compile: $(OBJ)

%.o: %.c fs-sim.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)


