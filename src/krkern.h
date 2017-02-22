#ifndef __KRKERN_H
#define __KRKERN_H

#include "kring.h"

struct ring_reader
{
	bool allocated;
};

struct ringset
{
	char name[KRING_NLEN];
	void *ctrl;
	struct kring_shared shared;
	struct page_desc *pd;
	wait_queue_head_t reader_waitqueue;
	
	bool has_writer;
	long num_readers;

	struct ring_reader reader[NRING_READERS];

	struct ringset *next;
};

struct kring_kern
{
	char name[KRING_NLEN];
	struct ringset *ringset;
	int rid;
};

int kring_wopen( struct kring_kern *kring, const char *ringset, int rid );
int kring_wclose( struct kring_kern *kring );
void kring_write( struct kring_kern *kring, int dir, void *d, int len );

#endif

