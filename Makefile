CFLAGS = -O0 -g -Wall -W
CXXFLAGS = $(CFLAGS)
CXXFLAGS_ADD = -std=gnu++03
DEFINES = -D_GNU_SOURCE
LDFLAGS =
LIBS =  -lebl -lelf -ldl -lrpm -lrpmio -lpq

all: schema.sql.inc
	g++ $(DEFINES) $(CXXFLAGS_ADD) $(CXXFLAGS) $(LDFLAGS) -o symboldb \
		symboldb.cpp \
		cpio_reader.cpp rpm_parser.cpp rpm_parser_exception.cpp \
		rpm_file_entry.cpp rpm_file_info.cpp rpmtd_wrapper.cpp \
		rpm_package_info.cpp \
		database.cpp database_exception.cpp \
		elf_symbol.cpp \
		elf_symbol_definition.cpp \
		elf_symbol_reference.cpp \
		elf_exception.cpp \
		elf_image.cpp \
		$(LIBS)

schema.sql.inc: schema.sql
	xxd -i < $< > $@
