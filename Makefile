# Executable Names
EXECUTABLE_atchim = atchim
EXECUTABLE_atchim_server = atchim_server
EXECUTABLE_assh = assh

# Compiler
CXX = clang++

# Compiler Flags
CXXFLAGS = -std=c++20 -Wall -Wextra -flto=thin -stdlib=libc++ -Iinclude

# Install paths
PREFIX ?= /usr/local
BINDIR = $(DESTDIR)$(PREFIX)/bin
DATADIR = $(DESTDIR)$(PREFIX)/share/atchim

# Libraries
LIBS_atchim = -lsqlite3
LIBS_atchim_server = -lpthread -lsqlite3
LIBS_assh = -lutil -luv

# Targets
all: $(EXECUTABLE_atchim) $(EXECUTABLE_atchim_server) $(EXECUTABLE_assh)

$(EXECUTABLE_atchim): src/atchim.cpp include/atchim/db.hpp include/atchim/zsh-init.tmpl
	$(CXX) $(CXXFLAGS) src/atchim.cpp -o $(EXECUTABLE_atchim) $(LIBS_atchim)

$(EXECUTABLE_atchim_server): src/atchim_server.cpp include/atchim/db.hpp
	$(CXX) $(CXXFLAGS) src/atchim_server.cpp -o $(EXECUTABLE_atchim_server) $(LIBS_atchim_server)

$(EXECUTABLE_assh): src/assh.cpp
	$(CXX) $(CXXFLAGS) src/assh.cpp -o $(EXECUTABLE_assh) $(LIBS_assh)

# Install
install: all
	mkdir -p $(BINDIR) $(DATADIR)
	install -m 755 $(EXECUTABLE_atchim) $(BINDIR)/
	install -m 755 $(EXECUTABLE_atchim_server) $(BINDIR)/
	install -m 755 $(EXECUTABLE_assh) $(BINDIR)/
	install -m 644 include/atchim/zsh-init.tmpl $(DATADIR)/
	install -m 644 -d themes $(DATADIR)/themes
	cp -r themes/* $(DATADIR)/themes/

uninstall:
	rm -f $(BINDIR)/$(EXECUTABLE_atchim)
	rm -f $(BINDIR)/$(EXECUTABLE_atchim_server)
	rm -f $(BINDIR)/$(EXECUTABLE_assh)
	rm -rf $(DATADIR)

clean:
	rm -f $(EXECUTABLE_atchim) $(EXECUTABLE_atchim_server) $(EXECUTABLE_assh)
