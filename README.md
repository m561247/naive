C compiler toolchain targeting Linux x64, written from scratch in C. Many
features are missing, but it is complete enough to self-host.

Includes:

* libc
* C frontend
* IR
* Assembly generation
* Assembler (for the in-memory format, and for textual assembly)
* ELF object file output
* Linker
* `ar` clone

Dependencies:

* make
* nasm
* ctags
