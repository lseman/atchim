# Executable Names
EXECUTABLE_atchim = build/atchim
EXECUTABLE_atchim_server = build/atchim_server
EXECUTABLE_assh = build/assh

# Compiler
CXX ?= c++

# Compiler Flags
CXXFLAGS ?= -std=c++23 -O2 -Wall -Wextra -Iinclude

# Install paths
PREFIX ?= /usr/local
BINDIR = $(DESTDIR)$(PREFIX)/bin
DATADIR = $(DESTDIR)$(PREFIX)/share/atchim

# CPM fetches a pinned FTXUI release on the first build.
# Set CPM_SOURCE_CACHE to share cached sources between build directories.
CMAKE ?= cmake
BUILD_DIR ?= build/cmake
JOBS ?= 4

.PHONY: all
all:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="$(CXX)" -DCMAKE_CXX_FLAGS="$(CXXFLAGS)" -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$(abspath build)"
	$(CMAKE) --build $(BUILD_DIR) --parallel $(JOBS)

# Install
install: all
	mkdir -p $(BINDIR) $(DATADIR)
	install -m 755 $(EXECUTABLE_atchim) $(BINDIR)/
	install -m 755 $(EXECUTABLE_atchim_server) $(BINDIR)/
	install -m 755 $(EXECUTABLE_assh) $(BINDIR)/
	install -d $(DATADIR)/themes
	cp -r themes/* $(DATADIR)/themes/

uninstall:
	rm -f $(BINDIR)/$(notdir $(EXECUTABLE_atchim))
	rm -f $(BINDIR)/$(notdir $(EXECUTABLE_atchim_server))
	rm -f $(BINDIR)/$(notdir $(EXECUTABLE_assh))
	rm -rf $(DATADIR)

clean:
	$(CMAKE) --build $(BUILD_DIR) --target clean

.PHONY: all install uninstall clean test
test: all
	python3 tests/integration.py
	python3 tests/picker.py
