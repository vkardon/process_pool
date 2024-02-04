
# Executable to build
EXE = app 

# Sources
PROJECT_HOME = .
OBJ_DIR = $(PROJECT_HOME)/_obj

SRCS = $(PROJECT_HOME)/Example.cpp \
       $(PROJECT_HOME)/ProcessPool.cpp

# Include directories
INCS = -I$(PROJECT_HOME)

# Libraries
LIBS =

# Objective files to build
OBJS = $(addprefix $(OBJ_DIR)/, $(addsuffix .o, $(basename $(notdir $(SRCS)))))

# Compiler and linker to use
CC = g++
CFLAGS = -std=gnu++17 -Wall
#CFLAGS = -std=gnu++11 -Wall -O3 -s -DNDEBUG

LD = $(CC)
LDFLAGS =

# Build executable
$(EXE): $(OBJS)
	$(LD) $(LDFLAGS) -o $(EXE) $(OBJS) $(LIBS)

# Compile source files
# Add -MP to generate dependency list
# Add -MMD to not include system headers
$(OBJ_DIR)/%.o: $(PROJECT_HOME)/%.cpp Makefile
	-mkdir -p $(OBJ_DIR)
	$(CC) -c -MP -MMD $(CFLAGS) $(INCS) -o $(OBJ_DIR)/$*.o $<
	
# Delete all intermediate files
clean clear:
	rm -rf $(EXE) $(OBJ_DIR) core

# Read the dependency files.
# Note: use '-' prefix to don't display error or warning
# if include file do not exist (just remade it)
-include $(OBJS:.o=.d)

