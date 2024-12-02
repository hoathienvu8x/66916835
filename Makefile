CC = gcc
LDFLAGS = -I. -ldl -lpthread -lm
ifeq ($(build),release)
	CFLAGS = -O3
	LDFLAGS += -DNDEBUG=1
else
	CFLAGS = -Og -g
endif
CFLAGS += -std=gnu99 -Wall -Wextra -Werror -pedantic
RM = rm -rf

OBJECTS =
OBJECTS := $(addprefix objects/,$(OBJECTS))

EXECUTABLE = server

all: objects $(EXECUTABLE)

objects:
	@echo "Create 'objects' folder ..."
	@mkdir -p objects

$(EXECUTABLE): objects/server.o $(OBJECTS)
ifeq ($(build),release)
	@echo "Build release '$@' executable ..."
else
	@echo "Build '$@' executable ..."
endif
	@$(CC) objects/server.o $(OBJECTS) -o $@ $(LDFLAGS)
	@$(RM) objects/server.o

objects/%.o: %.c
	@echo "Build $@ ..."
	@$(CC) -c $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	@echo "Cleanup ..."
	@$(RM) $(OBJECTS) $(EXECUTABLE)
