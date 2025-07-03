# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -pedantic
LDFLAGS = -lrt -pthread

# Object file for the dungeon
DUNGEON_OBJ = dungeon.o

# Targets
all: game barbarian wizard rogue

# Link game against dungeon object file
game: game.c $(DUNGEON_OBJ) dungeon_info.h
	$(CC) $(CFLAGS) game.c $(DUNGEON_OBJ) -o $@ $(LDFLAGS)

barbarian: barbarian.c dungeon_info.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

wizard: wizard.c dungeon_info.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

rogue: rogue.c dungeon_info.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f game barbarian wizard rogue 

