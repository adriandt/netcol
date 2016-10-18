#include "thread.h"
#include <stdlib.h>
#include <time.h>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

long Thread::enabledRealms = 0;
pthread_key_t Thread::thisKey;

namespace genf
{
	lfdostream *lf;
}

ItWriter::ItWriter()
:
	writer(0),
	reader(0),
	queue(0),
	id(-1),
	hblk(0), tblk(0),
	hoff(0), toff(0),
	toSend(0)
{
}

ItQueue::ItQueue( int blockSz )
:
	head(0), tail(0),
	blockSz(blockSz)
{
	pthread_mutex_init( &mutex, 0 );
	pthread_cond_init( &cond, 0 );

	free = 0;
}

ItWriter *ItQueue::registerWriter( Thread *writer, Thread *reader )
{
	ItWriter *itWriter = new ItWriter;
	itWriter->writer = writer;
	itWriter->reader = reader;
	itWriter->queue = this;

	/* Reigster under lock. */
	pthread_mutex_lock( &mutex );

	/* Allocate an id (index into the vector of writers). */
	for ( int i = 0; i < (int)writerVect.size(); i++ ) {
		/* If there is a free spot, use it. */
		if ( writerVect[i] == 0 ) {
			writerVect[i] = itWriter;
			itWriter->id = i;
			goto set;
		}
	}

	/* No Existing index to use. Append. */
	itWriter->id = writerVect.size();
	writerVect.push_back( itWriter );

set:
	writerList.append( itWriter );

	pthread_mutex_unlock( &mutex );
	return itWriter;
}

ItBlock *ItQueue::allocateBlock( int supporting )
{
	int size = ( supporting > IT_BLOCK_SZ ) ? supporting : IT_BLOCK_SZ;
	char *bd = new char[sizeof(ItBlock) + size];
	ItBlock *block = (ItBlock*) bd;
	block->data = bd + sizeof(ItBlock);
	block->size = size;
	block->prev = 0;
	block->next = 0;
	return block;
}

void ItQueue::freeBlock( ItBlock *block )
{
	delete block;
}

void *ItQueue::allocBytes( ItWriter *writer, int size )
{
	if ( writer->tblk == 0 ) {
		/* There are no blocks. */
		writer->hblk = writer->tblk = allocateBlock( size );
		writer->hoff = writer->toff = 0;
	}
	else {
		int avail = writer->tblk->size - writer->toff;

		/* Move to the next block? */
		if ( size > avail ) {
			ItBlock *block = allocateBlock( size );
			writer->tblk->next = block;
			writer->tblk = block;
			writer->toff = 0;

			/* Need to track the padding in the message length. */
			writer->mlen += avail;
		}
	}

	void *ret = writer->tblk->data + writer->toff;
	writer->toff += size;
	writer->mlen += size;
	return ret;
}

void ItQueue::send( ItWriter *writer )
{
	pthread_mutex_lock( &mutex );

	/* Stash the total length in the header. This grows after opening as
	 * variable-length fields are populated. */
	writer->toSend->length = writer->mlen;

	/* Put on the end of the message list. */
	if ( head == 0 ) {
		head = tail = writer->toSend;
		head->next = 0;
	}
	else {
		tail->next = writer->toSend;
		tail = writer->toSend;
	}


	/* Notify anyone waiting. */
	pthread_cond_broadcast( &cond );

	pthread_mutex_unlock( &mutex );

	if ( writer->reader->recvRequiresSignal )
		pthread_kill( writer->reader->pthread, SIGUSR1 );

}

ItHeader *ItQueue::wait()
{
	pthread_mutex_lock( &mutex );

	while ( head == 0 )
		pthread_cond_wait( &cond, &mutex );

	ItHeader *header = head;
	head = head->next;

	pthread_mutex_unlock( &mutex );

	header->next = 0;
	return header;
}

bool ItQueue::poll()
{
	pthread_mutex_lock( &mutex );
	bool result = head != 0;
	pthread_mutex_unlock( &mutex );

	return result;
}


void ItQueue::release( ItHeader *header )
{
	ItWriter *writer = writerVect[header->writerId];
	int length = header->length;

	/* Skip whole blocks. */
	int remaining = writer->hblk->size - writer->hoff;
	while ( length > 0 && length >= remaining ) {
		/* Pop the block. */
		ItBlock *pop = writer->hblk;
		writer->hblk = writer->hblk->next;
		writer->hoff = 0;

		/* Maybe we took the list to zero size. */
		if ( writer->hblk == 0 ) {
			writer->tblk = 0;
			writer->toff = 0;
		}

		freeBlock( pop );

		/* Take what was left off the length. */
		length -= remaining;

		/* Remaining is the size of the next block, if present. Always starting
		 * at hoff 0 when we move to the next block. */
		remaining = writer->hblk != 0 ? writer->hblk->size : 0;
	}

	/* Final move ahead. */
	writer->hoff += length;
};

int Thread::inetListen( uint16_t port )
{
	/* Create the socket. */
	int listenFd = socket( PF_INET, SOCK_STREAM, 0 );
	if ( listenFd < 0 ) {
		log_ERROR( "unable to allocate socket" );
		return -1;
	}

	/* Set its address to reusable. */
	int optionVal = 1;
	setsockopt( listenFd, SOL_SOCKET, SO_REUSEADDR,
			(char*)&optionVal, sizeof(int) );

	/* bind. */
	sockaddr_in sockName;
	sockName.sin_family = AF_INET;
	sockName.sin_port = htons(port);
	sockName.sin_addr.s_addr = htonl (INADDR_ANY);
	if ( bind(listenFd, (sockaddr*)&sockName, sizeof(sockName)) < 0 ) {
		log_ERROR( "unable to bind to port " << port );
		close( listenFd );
		return -1;
	}

	/* listen. */
	if ( listen( listenFd, 1 ) < 0 ) {
		log_ERROR( "unable put socket in listen mode" );
		close( listenFd );
		return -1;
	}

	return listenFd;
}

int Thread::pselectLoop( sigset_t *sigmask )
{
	/* accept loop. */
	while ( !breakLoop ) {
		/* Construct event sets. */
		fd_set readSet, writeSet;
		FD_ZERO( &readSet );
		FD_ZERO( &writeSet );
		int highest = -1;
		for ( SelectFdList::Iter fd = selectFdList; fd.lte(); fd++ ) {
			if ( fd->fd > highest )
				highest = fd->fd;

			if ( fd->wantRead )
				FD_SET( fd->fd, &readSet );

			if ( fd->wantWrite )
				FD_SET( fd->fd, &writeSet );

			/* Use this opportunity to reset the round abort flag. */
			fd->abortRound = false;
		}

		/* Wait no longer than a second. */
		timespec ts;
		ts.tv_nsec = 0;
		ts.tv_sec = 1;

		int result = pselect( highest+1, &readSet, &writeSet, 0, &ts, sigmask );

		if ( result < 0 ) {
			if ( errno == EINTR ) {
				poll();
				return -1;
			}

			if ( errno != EAGAIN ) 
				log_FATAL( "select returned an unexpected error " << strerror(errno) );
		}

		if ( result > 0 ) {
			for ( SelectFdList::Iter fd = selectFdList; fd.lte(); fd++ ) {
				/* Check for round abort on the FD. */
				if ( fd->abortRound )
					continue;

				uint8_t readyField = 0;
				if ( FD_ISSET( fd->fd, &readSet ) )
					readyField |= READ_READY;
				if ( FD_ISSET( fd->fd, &writeSet ) )
					readyField |= WRITE_READY;

				if ( readyField )
					selectFdReady( fd, readyField );
			}
		}

		poll();
	}

	/* finalTimerRun( c ); */
	/* close( listenFd ); */

	return 0;
}

int Thread::selectLoop()
{
	bool cont = true;
	while ( cont ) {
		int r = pselectLoop( 0 );
		if ( r == 0 )
			break;
	}
	return 0;
}

int Thread::inetConnect( const char *host, uint16_t port )
{
	sockaddr_in servername;
	hostent *hostinfo;
	long connectRes;

	/* Create the socket. */
	int fd = socket( PF_INET, SOCK_STREAM, 0 );
	if ( fd < 0 )
		log_ERROR( "SocketConnectFailed" );

	/* Lookup the host. */
	servername.sin_family = AF_INET;
	servername.sin_port = htons(port);
	hostinfo = gethostbyname( host );
	if ( hostinfo == NULL ) {
		::close( fd );
		log_ERROR( "SocketConnectFailed" );
	}

	servername.sin_addr = *(in_addr*)hostinfo->h_addr;

	/* Connect to the listener. */
	connectRes = ::connect( fd, (sockaddr*)&servername, sizeof(servername) );
	if ( connectRes < 0 ) {
		::close( fd );
		log_ERROR( "SocketConnectFailed" );
		return -1;
	}

	return fd;
}


void *thread_start_routine( void *arg )
{
	Thread *thread = (Thread*)arg;
	thread->tid = syscall( SYS_gettid );
	thread->setThis();
	long r = thread->start();
	return (void*)r;
}

std::ostream &operator <<( std::ostream &out, const log_lock & )
{
	lfdostream *fdo = dynamic_cast<lfdostream*>( &out );
	if ( fdo )
		pthread_mutex_lock( &fdo->mutex );
	return out;
}

std::ostream &operator <<( std::ostream &out, const log_unlock & )
{
	lfdostream *fdo = dynamic_cast<lfdostream*>( &out );
	if ( fdo )
		pthread_mutex_unlock( &fdo->mutex );
	return out;
}

std::ostream &operator <<( std::ostream &out, const Thread::endp & )
{
	exit( 1 );
}

std::ostream &operator <<( std::ostream &out, const log_time & )
{
	time_t epoch;
	struct tm local;
	char string[64];

	epoch = time(0);
	localtime_r( &epoch, &local );
	int r = strftime( string, sizeof(string), "%Y-%m-%d %H:%M:%S", &local );
	if ( r > 0 )
		out << string;

	return out;
}

std::ostream &operator <<( std::ostream &out, const log_prefix & )
{
	Thread *thread = Thread::getThis();
	if ( thread != 0 )
		out << *thread;
	else
		out << log_time() << ": ";
	return out;
}

std::ostream &operator <<( std::ostream &out, const Thread &thread )
{
	out << log_time() << ": " << thread.type << "(" << thread.tid << "): ";
	return out;
}
