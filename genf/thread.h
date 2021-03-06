#ifndef _SERVGEN_THREAD_H
#define _SERVGEN_THREAD_H

#include <iostream>
#include <pthread.h>
#include <vector>
#include <stdio.h>
#include <unistd.h>
#include <genf/list.h>
#include <stdint.h>
#include <sys/time.h>

#include "list.h"
#include <aapl/vector.h>
#include <aapl/rope.h>

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include <ares.h>
#include <arpa/nameser.h>

#define IT_BLOCK_SZ 4098

/* TLS */
#define EC_SOCKET_CONNECT_FAILED        104
#define EC_SSL_PEER_FAILED_VERIFY       100
#define EC_SSL_CONNECT_FAILED           105
#define EC_SSL_WRONG_HOST               106
#define EC_SSL_CA_CERT_LOAD_FAILURE     107
#define EC_SSL_NEW_CONTEXT_FAILURE      120
#define EC_SSL_CA_CERTS_NOT_SET         121
#define EC_SSL_ACCEPT_FAILED            152
#define EC_SSL_FAILED_TO_LOAD           159
#define EC_SSL_PARAM_NOT_SET            160
#define EC_SSL_CONTEXT_CREATION_FAILED  161
#define EC_NONBLOCKING_IO_NOT_AVAILABLE 162
#define EC_FCNTL_QUERY_FAILED           163
#define EC_WRITE_ERROR                  164
#define EC_DAEMON_DAEMON_TIMEOUT        165
#define EC_SOCKADDR_ERROR               166
#define EC_SOCK_NOT_LOCAL               167
#define EC_CONF_PARSE_ERROR             168
#define EC_WRITE_ON_NULL_SOCKET_BIO     169

#define DBG_SHIFT 56
#define DBG_LOW_MASK 0xfffffff
#define NUM_APP_ID 64

#define DBG_APP_ID(realm) ( realm >> DBG_SHIFT )
#define DBG_REALM(realm) ( realm & DBG_LOW_MASK )

#define DBG_THREAD     ( ( 0ull << DBG_SHIFT ) | 0x00000001 )
#define DBG_PACKET     ( ( 0ull << DBG_SHIFT ) | 0x00000002 )
#define DBG_CONNECTION ( ( 0ull << DBG_SHIFT ) | 0x00000004 )
#define DBG_TLS        ( ( 0ull << DBG_SHIFT ) | 0x00000008 )

/* user-allocated start: 16 */

struct ItWriter;
struct ItQueue;
struct Thread;
struct PacketWriter;
struct Recv;

struct ItHeader
{
	unsigned short msgId;
	unsigned short writerId;
	unsigned int length;
	ItHeader *next;
};

void *itqOpen( ItWriter *writer, int ID, size_t SZ );
void *itqRead( ItQueue *queue, ItHeader *header, size_t SZ );

struct ItBlock
{
	char *data;
	unsigned int size;

	ItBlock *prev, *next;
};

struct OptStringEl
{
	const char *data;
	OptStringEl *next;
};

struct OptStringList
{
	OptStringList() : head(0), tail(0) {}

	OptStringEl *head, *tail;
};

struct ItWriter
{
	ItWriter();

	Thread *writer;
	Thread *reader;
	ItQueue *queue;
	int id;

	/* Write to the tail block, at tail offset. */
	ItBlock *hblk;
	ItBlock *tblk;

	/* Head and tail offset. */
	int hoff;
	int toff;

	int mlen;

	ItHeader *toSend;
	void *contents;

	ItWriter *prev, *next;
};

typedef List<ItWriter> ItWriterList;
typedef std::vector<ItWriter*> ItWriterVect;

struct ItQueue
{
	ItQueue( int blockSz = IT_BLOCK_SZ );

	void *allocBytes( ItWriter *writer, int size );
	void send( ItWriter *writer, bool sendSignal );

	ItHeader *wait();
	bool poll();
	void release( ItHeader *header );

	pthread_mutex_t mutex;
	pthread_cond_t cond;

	ItHeader *head, *tail;
	int blockSz;

	ItBlock *allocateBlock( int needed );
	void freeBlock( ItBlock *block );

	ItWriter *registerWriter( Thread *writer, Thread *reader );

	/* Free list for blocks. */
	ItBlock *free;

	/* The list of writers in the order of registration. */
	ItWriterList writerList;

	/* A vector for finding writers. This lets us identify the writer in the
	 * message header with only a byte. */
	ItWriterVect writerVect;
};

namespace Message
{
	struct PacketPassthru
	{
		static PacketPassthru *open( ItWriter *writer )
		{
			return (PacketPassthru*)itqOpen( writer,
					PacketPassthru::ID, sizeof(PacketPassthru) );
		}

		static PacketPassthru *read( ItQueue *queue, ItHeader *header )
		{
			return (PacketPassthru*)itqRead( queue, header, sizeof(PacketPassthru) );
		}

		void *rope;
		static const unsigned short ID = 1;
	};
}

struct SelectFd
{
	enum Type {
		User = 1,
		Listen,
		Connection,
		Process
	};

	enum State {
		Lookup = 1,
		Connect,
		TlsAccept,
		TlsConnect,
		TlsEstablished,
		Established
	};

	SelectFd( Thread *thread, int fd, void *local )
	:
		type(User),
		thread(thread),
		fd(fd),
		local(local),
		wantRead(false),
		wantWrite(false),
		ssl(0),
		bio(0),
		remoteHost(0),
		sslVerifyError(false),
		closed(false),
		tlsWantRead(false),
		tlsWantWrite(false),
		tlsEstablished(false),
		tlsWriteWantsRead(false),
		tlsReadWantsWrite(false),
		port(0)
	{}

	bool wantReadGet()
	{
		return ( !tlsEstablished ) ? wantRead :
				( tlsWantRead || ( tlsWantWrite && tlsWriteWantsRead ) );
	}

	bool wantWriteGet()
	{
		return ( !tlsEstablished ) ? wantWrite :
				( tlsWantWrite || ( tlsWantRead && tlsReadWantsWrite ) );
	}

	void wantReadSet( bool v )
	{
		if ( !tlsEstablished )
			wantRead = v;
		else
			tlsWantRead = v;
	}

	void wantWriteSet( bool v )
	{
		if ( !tlsEstablished )
			wantWrite = v;
		else
			tlsWantWrite = v;
	}

	Type type;
	State state;
	Thread *thread;
	int fd;
	void *local;
	bool wantRead;
	bool wantWrite;
	SSL *ssl;
	BIO *bio;
	const char *remoteHost;
	bool sslVerifyError;
	bool closed;

	/* If connection is tls, the application should use these. */
	bool tlsWantRead;
	bool tlsWantWrite;

	bool tlsEstablished;

	bool tlsWriteWantsRead;
	bool tlsReadWantsWrite;

	unsigned short port;

	SelectFd *prev, *next;
};

typedef List<SelectFd> SelectFdList;

struct Connection
{
	enum FailType {
		SslReadFailure = 1,
		SslPeerFailedVerify,
		SslPeerCnHostMismatch,
		SslAcceptError,
		AsyncConnectFailed,
		LookupFailure
	};

	Connection( Thread *thread );

	virtual void failure( FailType failType ) = 0;
	virtual void connectComplete() = 0;
	virtual void notifyAccept() = 0;
	virtual void readReady() = 0;
	virtual void writeReady() = 0;

	Thread *thread;
	SelectFd *selectFd;

	bool tlsConnect;
	SSL_CTX *sslCtx;
	bool checkHost;

	bool closed;
	bool onSelectList;

	void close();

	void initiate( sockaddr_in *sa, bool tls, SSL_CTX *sslCtx, bool checkHost );
	void initiate( const char *host, uint16_t port, bool tls, SSL_CTX *sslCtx, bool checkHost );

	bool isEstablished()
	{
		return selectFd != 0 &&
			( selectFd->state == SelectFd::Established ||
			selectFd->state == SelectFd::TlsEstablished );
	}

	/* -1: EOF, 0: try again, pos: data. */
	int read( char *data, int len );
	int write( char *data, int len );
};

/* Connection factory. */
struct Listener
{
	Listener( Thread *thread );

	Thread *thread;
	SelectFd *selectFd;
	bool tlsAccept;
	SSL_CTX *sslCtx;
	bool checkHost;

	virtual Connection *connectionFactory( int fd ) = 0;

	void startListenOnFd( int fd, bool tls, SSL_CTX *sslCtx, bool checkHost );
	void startListen( unsigned short port, bool tls, SSL_CTX *sslCtx, bool checkHost );
};

struct WriteFuncs
{
	virtual void bufClose( SelectFd *selectFd ) = 0;
	virtual int bufWrite(  SelectFd *selectFd, char *data, int len ) = 0;
};

struct WriteBuffer
{
	WriteBuffer( WriteFuncs *writeFuncs )
		: writeFuncs(writeFuncs), closeOnFlushed(false) {}

	int write( SelectFd *selectFd, char *data, int len );
	void send( SelectFd *selectFd, Rope &blocks, bool canConsume );
	void send( SelectFd *selectFd, char *data, int length );
	void writeReady( SelectFd *selectFd );
	void sendEos( SelectFd *selectFd );

	WriteFuncs *writeFuncs;
	Rope queue;
	bool closeOnFlushed;
};

struct Process
:
	public WriteFuncs
{
	Process( Thread *thread, const char *cmdline )
	:
		cmdline( cmdline ),
		from( thread, -1, 0 ),
		to( thread, -1, 0 ),
		writeBuffer( this )
	{}

	const char *cmdline;
	pid_t pid;

	SelectFd from;
	SelectFd to;

	WriteBuffer writeBuffer;

	void wantWriteSet( bool value )
		{ to.wantWriteSet( value ); }
	bool wantWriteGet()
		{ return to.wantWriteGet(); }

	void wantReadSet( bool value )
		{ from.wantReadSet( value ); }
	bool wantReadGet()
		{ return from.wantReadGet(); }
	
	virtual void readReady( SelectFd *fd ) {}
	virtual void writeReady( SelectFd *fd ) {}

	virtual int bufWrite( SelectFd *selectFd, char *data, int len ) { return writeBuffer.write( &to, data, len ); }
	virtual void bufClose( SelectFd *selectFd ) { ::close( to.fd ); to.closed = false; }
};

struct Thread
{
	Thread( const char *program, const char *type )
	:
		program( program ),
		type( type ),
		pthread_this( 0 ),
		pthread_parent( 0 ),
		tid( 0 ),
		recvRequiresSignal( false ),
		pendingNotifSignal( false ),
		logFile( &std::cerr ),
		selectTimeout( 0 ),
		loop( true )
	{
	}

	Thread()
	{
	}

	const char *program;
	const char *type;
	struct endp {};
	typedef List<Thread> ThreadList;

	/* First value is for use by child and is retrieved on startup, second is
	 * for use by parent and is set by pthread_create. */
	pthread_t pthread_this;
	pthread_t pthread_parent;
	pid_t tid;

	ares_channel ac;

	/* Set this true in a thread's constructor if the main loop is not driven
	 * by listening for genf messages. Signals will be sent automatically on
	 * message send. */
	bool recvRequiresSignal;

	/* If a sender tries to signal this thread before it has started up and
	 * properly set it's self paramter then this will be tripped and the thread
	 * will send itself the signal in order to break from any syscal and handle
	 * the message. */
	bool pendingNotifSignal;

	std::ostream *logFile;
	ItQueue control;

	ThreadList childList;

	/* Default timeout for select. This is not a timer, but rather a maximum
	 * time to wait in select call. Unlike a timer it resets with every run
	 * around the loop. Only used if there is no timer. No specific action can
	 * be triggered, just causes a poll of the message queue. Default value of
	 * zero means no timeout. */
	int selectTimeout;

protected:
	bool loop;

public:
	Thread *prev, *next;

	void breakLoop()
		{ loop = false; }
	void loopBreak()
		{ loop = false; }
	bool loopContinue()
		{ return loop; }
	void loopBegin()
		{ loop = true; }

	virtual int start() = 0;

	void funnelSigs( sigset_t *set );

	const Thread &log_prefix() { return *this; }

	virtual const char *pkgDataDir() = 0;
	virtual const char *pkgStateDir() = 0;
	virtual const char *pkgConfDir() = 0;

	virtual	bool poll() = 0;
	int inetListen( uint16_t port, bool transparent = false );
	int selectLoop( timeval *timer = 0, bool wantPoll = true );

	int pselectLoop( sigset_t *sigmask, timeval *timer, bool wantPoll );
	int inetConnect( sockaddr_in *sa, bool nonBlocking );

	virtual void recvSingle() {}

	/* Handle select FD ready for user mode connections. */
	virtual void userFdReady( SelectFd *selectFd, uint8_t readyField ) {}

	void listenReady( SelectFd *fd, uint8_t readyMask );
	void connConnectReady( SelectFd *fd, uint8_t readyMask );
	void connTlsAcceptReady( SelectFd *fd );
	void connTlsConnectReady( SelectFd *fd );
	void connTlsEstablishedReady( SelectFd *fd, uint8_t readyMask );
	void connEstablishedReady( SelectFd *fd, uint8_t readyMask );
	void selectFdReady( SelectFd *selectFd, uint8_t readyField );

	virtual void handleTimer() {};
	virtual void handleSignal( int sig ) {}

	static const uint8_t READ_READY  = 0x01;
	static const uint8_t WRITE_READY = 0x02;

	SelectFdList selectFdList;

	static pthread_key_t thisKey;

	static Thread *getThis()
		{ return (Thread*) pthread_getspecific( thisKey ); }

	/* Must be called from the thread this struct represents before it is run. */
	void initId();

	typedef unsigned long long RealmSet;
	static RealmSet enabledRealms[];

	int signalLoop( sigset_t *set, struct timeval *timer = 0 );

	virtual void dispatchPacket( SelectFd *fd, Recv &recv ) {}

	/*
	 * SSL
	 */

	SSL_CTX *sslCtxClientPublic();
	SSL_CTX *sslCtxClientInternal();
	SSL_CTX *sslCtxClient( const SSL_METHOD *method, const char *verify, const char *key = 0, const char *cert = 0 );

	SSL_CTX *sslCtxServerInternal();
	SSL_CTX *sslCtxServer( const char *key, const char *cert, const char *verify = 0 );
	SSL_CTX *sslCtxServer( EVP_PKEY *pkey, X509 *x509 );

	bool makeNonBlocking( int fd );

	void startTlsServer( SSL_CTX *defaultCtx, SelectFd *selectFd );
	void startTlsClient( SSL_CTX *clientCtx, SelectFd *selectFd, const char *remoteHost );

	void asyncLookup( SelectFd *fd, const char *host )
		{ asyncLookupQuery( fd, host ); }

	void asyncLookupHost( SelectFd *fd, const char *host );
	void asyncLookupQuery( SelectFd *fd, const char *host );

	void lookupCallbackQuery( SelectFd *fd, int status, int timeouts,
			unsigned char *abuf, int alen );
	void lookupCallbackHost( SelectFd *fd, int status, int timeouts,
			struct hostent *hostent );

	int checkName( const char *name, ASN1_STRING *pattern );
	bool hostMatch( X509 *cert, const char *name );
	bool hostMatch( SelectFd *selectFd, const char *name );

	int tlsWrite( SelectFd *fd, char *data, int len );
	int tlsRead( SelectFd *fd, void *buf, int len );

	void tlsStartup( const char *randFile = 0 );
	void tlsShutdown();

	void tlsError( RealmSet realm, int e );

	bool prepNextRound( SelectFd *fd, int result );

	static char *pktFind( Rope *rope, long l );

	virtual void recvPassthru( Message::PacketPassthru *msg ) {}

	int createProcess( Process *proc );
};

struct MainBase
:
	public Thread
{
	MainBase( const char *package, const char *type )
		: Thread( package, type ) {}

	void handleSignal( int sig );
	void signalSetup();
	int signalLoop( struct timeval *timer = 0 );
	int selectLoop( timeval *timer = 0, bool wantPoll = true );
};

extern "C" void *genf_thread_start( void *arg );
void thread_funnel_handler( int s );

struct log_prefix { };
struct log_time { };

struct fdoutbuf
:
	public std::streambuf
{
	fdoutbuf( int fd )
	:
		fd(fd)
	{
	}

	void send( const char *s, std::streamsize num )
	{
		for ( std::streamsize i = 0; i < num; i++ ) {
			if ( s[i] == '\n' ) {
				buffer.append( s, i+1 );
				write( fd, buffer.data, buffer.length() );
				buffer.empty();

				i += 1;
				if ( i < num )
					buffer.append( s + i, num - i );
				
				return;
			}
		}

		buffer.append( s, num );
	}

	int_type overflow( int_type c )
	{
		if ( c != EOF ) {
			char z = c;
			send( &z, 1 );
		}
		return c;
	}

	std::streamsize xsputn( const char *s, std::streamsize num )
	{
		send( s, num );
		return num;
	}

	Vector<char> buffer;
	int fd;
};

struct lfdostream
:
	public std::ostream
{
	lfdostream( int fd )
	:
		std::ostream( 0 ),
		buf( fd )
	{
		pthread_mutex_init( &mutex, 0 );
		rdbuf( &buf );
	}

	pthread_mutex_t mutex;
	fdoutbuf buf;
};

struct log_lock {};
struct log_unlock {};

std::ostream &operator <<( std::ostream &out, const Thread::endp & );
std::ostream &operator <<( std::ostream &out, const log_lock & );
std::ostream &operator <<( std::ostream &out, const log_unlock & );
std::ostream &operator <<( std::ostream &out, const log_time & );

std::ostream &operator <<( std::ostream &out, const log_prefix & );
std::ostream &operator <<( std::ostream &out, const Thread &thread );

namespace genf
{
	extern lfdostream *lf;
}

struct log_array
{
	log_array( const char *data, int len )
	:
		data(data), len(len)
	{}

	log_array( const unsigned char *data, int len )
	:
		data((const char*)data),
		len(len)
	{}

	const char *data;
	int len;
};

#define log_text log_array

struct log_binary
{
	log_binary( const char *data, int len )
	:
		data(data), len(len)
	{}

	log_binary( const unsigned char *data, int len )
	:
		data((const char*)data),
		len(len)
	{}

	const char *data;
	int len;
};

struct log_hex
{
	log_hex( const char *data, int len )
		:
			data(data), len(len)
	{}

	log_hex( const unsigned char *data, int len )
		:
			data((const char*)data),
			len(len)
	{}

	const char *data;
	int len;
};

inline std::ostream &operator <<( std::ostream &out, const log_text &a )
{
	out.write( a.data, a.len );
	return out;
}

std::ostream &operator <<( std::ostream &out, const log_binary &b );
std::ostream &operator <<( std::ostream &out, const log_hex &b );

/* FIXME: There is a gotcha here. The class-specific of log_prefix does not
 * work with static functions. */

/* The log_prefix() expression can reference a struct or a function that
 * returns something used to write a different prefix. The macros don't care.
 * This allows for context-dependent log messages. */

#define log_FATAL( msg ) \
	*genf::lf << log_lock() << "FATAL:   " << log_prefix() << \
	msg << std::endl << log_unlock() << Thread::endp()

#define log_ERROR( msg ) \
	*genf::lf << log_lock() << "ERROR:   " << log_prefix() << \
	msg << std::endl << log_unlock()
	
/* Phase out. */
#define log_message( msg ) \
	*genf::lf << log_lock() << "message: " << log_prefix() << \
	msg << std::endl << log_unlock()

#define log_warning( msg ) \
	*genf::lf << log_lock() << "WARNING: " << log_prefix() << \
	msg << std::endl << log_unlock()

#define log_WARNING( msg ) \
	*genf::lf << log_lock() << "WARNING: " << log_prefix() << \
	msg << std::endl << log_unlock()

#define log_debug( realm, msg ) \
	do { if ( Thread::enabledRealms[DBG_APP_ID(realm)] & ( DBG_REALM(realm) ) ) \
		*genf::lf << log_lock() << "debug: " << log_prefix() << \
		msg << std::endl << log_unlock(); } while(0)

#define log_enabled( realm ) \
	( Thread::enabledRealms[DBG_APP_ID(realm)] & DBG_REALM( realm ) )

#endif
