CC = gcc
FLAGS = -Og -g -Wall -fcommon
LIBFLAGS = -c
INCLUDE = -I. -Iinclude

DEVS = mem disk
UTILS = spinlock list
ALLOCS = slaballocator
LIBS = mobj vnode svfs
ALL = $(DEVS) $(UTILS) $(ALLOCS) $(LIBS) 

DEVOBJS = bin/mem.o bin/disk.o
UTILOBJS = bin/spinlock.o bin/list.o
ALLOCOBJS = bin/slab_allocator.o
LIBOBJS = bin/mobj.o bin/vnode.o bin/svfs.o
ALLOBJS = $(DEVOBJS) $(UTILOBJS) $(ALLOCOBJS) $(LIBOBJS) 

empty-iso:
	dd if=/dev/zero bs=4096 count=161 > sys/empty-fs.iso

fs: empty-iso
	python3 fs-maker.py

mem:
	$(CC) $(FLAGS) $(LIBFLAGS) $(INCLUDE) sys/mem/mem.c -o bin/mem.o

disk:
	$(CC) $(FLAGS) $(LIBFLAGS) $(INCLUDE) sys/disk/disk.c -o bin/disk.o

spinlock:
	$(CC) $(FLAGS) $(LIBFLAGS) $(INCLUDE) util/spinlock.c -o bin/spinlock.o

list:
	$(CC) $(FLAGS) $(LIBFLAGS) $(INCLUDE) util/list.c -o bin/list.o

string:
	$(CC) $(FLAGS) $(LIBFLAGS) $(INCLUDE) util/string.c -o bin/string.o

slaballocator:
	$(CC) $(FLAGS) $(LIBFLAGS) $(INCLUDE) src/slab_allocator.c -o bin/slab_allocator.o

mobj: $(UTILS)
	$(CC) $(FLAGS) $(LIBFLAGS) $(INCLUDE) src/mobj.c -o bin/mobj.o

vnode: $(UTILS)
	$(CC) $(FLAGS) $(LIBFLAGS) $(INCLUDE) src/vnode.c -o bin/vnode.o

svfs: $(UTILS) $(ALLOCS)
	$(CC) $(FLAGS) $(LIBFLAGS) $(INCLUDE) src/svfs.c -o bin/svfs.o

mobjtest: $(UTILS) mobj
	$(CC) $(FLAGS) $(LIBFLAGS) $(INCLUDE) test/mobj_test.c -o bin/mobj_test.o
	$(CC) $(FLAGS) $(UTILOBJS) bin/mobj.o bin/mobj_test.o -o bin/mobj_test

svfstest: $(ALL)
	$(CC) $(FLAGS) $(LIBFLAGS) $(INCLUDE) test/svfs_test.c -o bin/svfs_test.o
	$(CC) $(FLAGS) $(ALLOBJS) bin/svfs_test.o -o bin/svfs_test

namev:
	$(CC) $(FLAGS) $(LIBFLAGS) $(INCLUDE) src/namev.c -o bin/namev.o

clean:
	rm -f bin/*
