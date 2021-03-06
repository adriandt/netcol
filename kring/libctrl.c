#include "kring.h"
#include "libkring.h"

#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>


char *kctrl_error( struct kring_user *u, int err )
{
	int len;
	const char *prefix, *errnostr;

	prefix = "<unknown>";
	switch ( err ) {
		case KRING_ERR_SOCK:
			prefix = "socket call failed";
			break;
		case KRING_ERR_MMAP:
			prefix = "mmap call failed";
			break;
		case KRING_ERR_BIND:
			prefix = "bind call failed";
			break;
		case KRING_ERR_READER_ID:
			prefix = "getsockopt(reader_id) call failed";
			break;
		case KRING_ERR_WRITER_ID:
			prefix = "getsockopt(writer_id) call failed";
			break;
		case KRING_ERR_RING_N:
			prefix = "getsockopt(ring_n) call failed";
			break;
		case KRING_ERR_ENTER:
			prefix = "exception in ring entry";
			break;
	}

	/* start with the prefix. Always there (see above). */
	len = strlen(prefix);

	/* Maybe add errnostring. */
	if ( u->_errno != 0 ) {
		errnostr = strerror(u->_errno);
		if ( errnostr )
			len += 2 + strlen(errnostr);
	}

	/* Null. */
	len += 1;

	u->errstr = malloc( len );
	strcpy( u->errstr, prefix );

	if ( errnostr != 0 ) {
		strcat( u->errstr, ": " );
		strcat( u->errstr, errnostr );
	}

	return u->errstr;
}

static unsigned long cons_pgoff( unsigned long ring_id, unsigned long region )
{
	return (
		( ( ring_id << KCTRL_PGOFF_ID_SHIFT )    & KCTRL_PGOFF_ID_MASK ) |
		( ( region << KCTRL_PGOFF_REGION_SHIFT ) & KCTRL_PGOFF_REGION_MASK )
	) * KRING_PAGE_SIZE;
}

int kctrl_map_enter( struct kring_user *u, int ring_id, int ctrl )
{
	void *r;

	r = mmap( 0, KCTRL_CTRL_SZ, PROT_READ | PROT_WRITE,
			MAP_SHARED, u->socket,
			cons_pgoff( ring_id, KRING_PGOFF_CTRL ) );

	if ( r == MAP_FAILED ) {
		kctrl_func_error( KRING_ERR_MMAP, errno );
		return -1;
	}

	u->control->head = r + KCTRL_CTRL_OFF_HEAD;
	u->control->writer = r + KCTRL_CTRL_OFF_WRITER;
	u->control->reader = r + KCTRL_CTRL_OFF_READER;
	u->control->descriptor = r + KCTRL_CTRL_OFF_DESC;

	r = mmap( 0, KCTRL_DATA_SZ, PROT_READ | PROT_WRITE,
			MAP_SHARED, u->socket,
			cons_pgoff( ring_id, KRING_PGOFF_DATA ) );

	if ( r == MAP_FAILED ) {
		kctrl_func_error( KRING_ERR_MMAP, errno );
		return -1;
	}

	u->data[ctrl].page = (struct kring_page*)r;

	return 0;
}

void kctrl_lock( int *mutex, unsigned long long *spins )
{
	while ( __sync_lock_test_and_set( mutex, 1 ) ) {
		/* If builtin returns 1 we did not flip it and therefore did not acquire the lock. */
		__sync_add_and_fetch( spins, 1 );
	}
}

void kctrl_unlock( int *mutex )
{
	__sync_lock_release( mutex, 0 );
}

/*
 * NOTE: when open for writing we always are writing to a specific ring id. No
 * need to iterate over control and data or dereference control/data pointers.
 */
int kctrl_write_decrypted( struct kring_user *u, long id, int type, const char *remoteHost, char *data, int len )
{
#if 0
	struct kctrl_decrypted_header *h;
	unsigned char *bytes;
	char buf[1];

	if ( len > kctrl_decrypted_max_data()  )
		len = kctrl_decrypted_max_data();

	h = (struct kctrl_decrypted_header*) kctrl_write_FIRST( u );

	h->len = len;
	h->id = id;
	h->type = type;
	if ( remoteHost == 0 )
		h->host[0] = 0;
	else {
		strncpy( h->host, remoteHost, sizeof(h->host) );
		h->host[sizeof(h->host)-1] = 0;
	}   

	bytes = (unsigned char*)( h + 1 );
	memcpy( bytes, data, len );

	kctrl_write_SECOND( u );

	/* Wake up here. */
	send( u->socket, buf, 1, 0 );

#endif
	return 0;
}   

/*
 * NOTE: when open for writing we always are writing to a specific ring id. No
 * need to iterate over control and data or dereference control/data pointers.
 */
int kctrl_write_plain( struct kring_user *u, char *data, int len )
{
	struct kctrl_plain_header *h;
	unsigned char *bytes;
	char buf[1];

	if ( len > kctrl_plain_max_data()  )
		len = kctrl_plain_max_data();

	while ( 1 ) {
		h = (struct kctrl_plain_header*) kctrl_write_FIRST( u );
		if ( h != 0 )
			break;

		recv( u->socket, buf, 1, 1 );
	}

	h->len = len;

	bytes = (unsigned char*)( h + 1 );
	memcpy( bytes, data, len );

	kctrl_write_SECOND( u );

	return 0;
}   

int kctrl_read_wait( struct kring_user *u )
{
	char buf[1];
	int ret = recv( u->socket, buf, 1, 1 ); 
	if ( ret == -1 && errno == EINTR )
		ret = 0;
	return ret;
}

void kctrl_next_packet( struct kring_user *u, struct kctrl_packet *packet )
{
	struct kctrl_packet_header *h;
	char buf[1];

	h = (struct kctrl_packet_header*) kctrl_next_generic( u );

	send( u->socket, buf, 1, 0 );

	packet->len = h->len;
	packet->caplen = 
			( h->len <= kctrl_packet_max_data() ) ?
			h->len :
			kctrl_packet_max_data();
	packet->dir = h->dir;
	packet->bytes = (unsigned char*)( h + 1 );
}

void kctrl_next_decrypted( struct kring_user *u, struct kctrl_decrypted *decrypted )
{
	struct kctrl_decrypted_header *h;
	char buf[1];

	h = (struct kctrl_decrypted_header*) kctrl_next_generic( u );

	send( u->socket, buf, 1, 0 );

	decrypted->len = h->len;
	decrypted->id = h->id;
	decrypted->type = h->type;
	decrypted->host = h->host;
	decrypted->bytes = (unsigned char*)( h + 1 );
}

void kctrl_next_plain( struct kring_user *u, struct kctrl_plain *plain )
{
	struct kctrl_plain_header *h;
	char buf[1];

	h = (struct kctrl_plain_header*) kctrl_next_generic( u );

	send( u->socket, buf, 1, 0 );

	plain->len = h->len;
	plain->bytes = (unsigned char*)( h + 1 );
}


