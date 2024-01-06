# Compiler
CXX = clang++

# Compiler Flags
#CXXFLAGS = -std=c++20 -Wall -Wextra -flto=thin

# Executable Names
EXECUTABLE_atchim = atchim
EXECUTABLE_atchim_server = atchim_server
EXECUTABLE_assh = assh

# Libraries
LIBS_atchim = 
LIBS_atchim_server = -lpthread -lsqlite3
LIBS_assh = -lutil -luv

# Targets
all: $(EXECUTABLE_atchim) $(EXECUTABLE_atchim_server) $(EXECUTABLE_assh)

$(EXECUTABLE_atchim): src/atchim.cpp
	$(CXX) $(CXXFLAGS) src/atchim.cpp -o $(EXECUTABLE_atchim) $(LIBS_atchim)

$(EXECUTABLE_atchim_server): src/atchim_server.cpp
	$(CXX) $(CXXFLAGS) src/atchim_server.cpp -o $(EXECUTABLE_atchim_server) $(LIBS_atchim_server)

$(EXECUTABLE_assh): src/assh.cpp
	$(CXX) $(CXXFLAGS) src/assh.cpp -o $(EXECUTABLE_assh) $(LIBS_assh)

clean:
	rm -f $(EXECUTABLE_atchim) $(EXECUTABLE_atchim_server) $(EXECUTABLE_assh)
