#include "thread.h"

#include <openssl/ssl.h>
#include <errno.h>

Listener::Listener( Thread *thread )
:
	thread(thread),
	selectFd(0),
	tlsAccept(false),
	sslCtx(0),
	checkHost(false)
{
}

void Listener::startListenOnFd( int fd, bool tls, SSL_CTX *sslCtx, bool checkHost )
{
	selectFd = new SelectFd( thread, fd, this );

	this->tlsAccept = tls;
	this->sslCtx = sslCtx;
	this->checkHost = checkHost;

	selectFd->type = SelectFd::Listen;
	selectFd->fd = fd;
	selectFd->wantRead = true;

	thread->selectFdList.append( selectFd );
}

void Listener::startListen( unsigned short port, bool tls, SSL_CTX *sslCtx, bool checkHost )
{
	int fd = thread->inetListen( port );
	startListenOnFd( fd, tls, sslCtx, checkHost );
}

Connection::Connection( Thread *thread )
:
	thread(thread),
	selectFd(0),
	tlsConnect(false),
	sslCtx(0),
	checkHost(false),
	closed(false),
	onSelectList(false)
{
}

void Connection::initiate( sockaddr_in *addr, bool tls, SSL_CTX *sslCtx, bool checkHost )
{
	selectFd = new SelectFd( thread, -1, 0 );
	selectFd->local = this;

	this->tlsConnect = tls;
	this->sslCtx = sslCtx;
	this->checkHost = checkHost;

	selectFd->type = SelectFd::Connection;
	selectFd->state = SelectFd::Connect;

	int connFd = thread->inetConnect( addr, true );

	selectFd->fd = connFd;
	selectFd->wantWrite = true;
	onSelectList = true;
	thread->selectFdList.append( selectFd );
}

void Connection::initiate( const char *host, uint16_t port, bool tls,
		SSL_CTX *sslCtx, bool checkHost )
{
	selectFd = new SelectFd( thread, -1, 0 );
	selectFd->local = this;

	this->tlsConnect = tls;
	this->sslCtx = sslCtx;
	this->checkHost = checkHost;

	selectFd->type = SelectFd::Connection;
	selectFd->state = SelectFd::Lookup;
	selectFd->port = port;

	thread->asyncLookupHost( selectFd, host );
}

int Connection::read( char *data, int len )
{
	if ( tlsConnect ) {
		return thread->tlsRead( selectFd, data, len );
	}
	else {
		int res = ::read( selectFd->fd, data, len );
		if ( res < 0 ) {
			if ( errno == EAGAIN || errno == EWOULDBLOCK ) {
				/* Nothing now. */
				return 0;
			}
			else {
				/* closed. */
				return -1;
			}
		}
		else if ( res == 0 ) {
			/* Normal closure. */
			return -1;
		}

		return res;
	}
}

int Connection::write( char *data, int len )
{
	if ( tlsConnect ) {
		log_debug( DBG_CONNECTION, "TLS write of " << len << " bytes" );
		return thread->tlsWrite( selectFd, data, len );
	}
	else {
		log_debug( DBG_CONNECTION, "system write of " <<
				len << " bytest to fd " << selectFd->fd );
		int res = ::write( selectFd->fd, data, len );
		if ( res < 0 ) {
			if ( errno == EAGAIN || errno == EWOULDBLOCK ) {
				/* Cannot write anything now. */
				return 0;
			}
			else {
				/* error-based closure. */
				return -1;
			}
		}
		return res;
	}
}

void Connection::close( )
{
	if ( selectFd != 0 ) {
		if ( selectFd->ssl != 0 ) {
			SSL_shutdown( selectFd->ssl );
		    SSL_free( selectFd->ssl );
		}

		if ( selectFd->fd >= 0 )
		    ::close( selectFd->fd );

		selectFd->closed = true;
	}

	selectFd = 0;
	closed = true;
}

void Thread::listenReady( SelectFd *fd, uint8_t readyMask )
{
	sockaddr_in peer;
	socklen_t len = sizeof(sockaddr_in);

	log_debug( DBG_CONNECTION, "listen socket is ready" );

	int result = ::accept( fd->fd, (sockaddr*)&peer, &len );
	if ( result >= 0 ) {

		Listener *l = static_cast<Listener*>(fd->local);

		bool nb = makeNonBlocking( result );
		if ( !nb )
			log_ERROR( "pkt-listen, post-accept: non-blocking IO not available" );

		Connection *pc = l->connectionFactory( result );
		SelectFd *selectFd = new SelectFd( this, result, 0 );
		selectFd->local = static_cast<Connection*>(pc);
		pc->selectFd = selectFd;

		log_debug( DBG_CONNECTION, "accepted connection, tls mode: " << l->tlsAccept );

		if ( l->tlsAccept ) {
			pc->tlsConnect = true;
			pc->sslCtx = l->sslCtx;
			pc->checkHost = l->checkHost;
			startTlsServer( pc->sslCtx, selectFd );
			selectFd->type = SelectFd::Connection;
			selectFd->state = SelectFd::TlsAccept;
		}
		else {
			pc->tlsConnect = false;
			selectFd->type = SelectFd::Connection;
			selectFd->state = SelectFd::Established;
			selectFd->wantRead = true;
			selectFdList.append( selectFd );
			pc->notifyAccept();
		}
	}
	else {
		if ( errno != EAGAIN && errno != EWOULDBLOCK )
			log_ERROR( "failed to accept connection: " << strerror(errno) );
	}
}

void Thread::connConnectReady( SelectFd *fd, uint8_t readyMask )
{
	Connection *c = static_cast<Connection*>(fd->local);

	if ( readyMask & WRITE_READY ) {
		/* Turn off want write. We must do this before any notification
		 * below, which may want to turn it on. */
		fd->wantWrite = false;

		int option;
		socklen_t optlen = sizeof(int);
		getsockopt( fd->fd, SOL_SOCKET, SO_ERROR, &option, &optlen );
		if ( option == 0 ) {
			log_debug( DBG_CONNECTION, "async connect completed" );
			if ( c->tlsConnect ) {
				startTlsClient( c->sslCtx, fd, fd->remoteHost );
				fd->state = SelectFd::TlsConnect;
			}
			else {
				fd->state = SelectFd::Established;
				fd->wantRead = true;
				c->connectComplete();
			}
		}
		else {
			log_ERROR( "failed async connect: " << strerror(option) );
			c->failure( Connection::AsyncConnectFailed );
			c->close();
		}
	}
}

void Thread::connTlsEstablishedReady( SelectFd *fd, uint8_t readyMask )
{
	Connection *c = static_cast<Connection*>(fd->local);
	if ( fd->tlsWantRead )
		c->readReady();

	if ( fd->tlsWantWrite )
		c->writeReady();
}

void Thread::connEstablishedReady( SelectFd *fd, uint8_t readyMask )
{
	Connection *c = static_cast<Connection*>(fd->local);
	if ( readyMask & READ_READY )
		c->readReady();

	if ( readyMask & WRITE_READY && fd->wantWrite )
		c->writeReady();
}

