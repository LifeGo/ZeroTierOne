/*
 * ZeroTier One - Network Virtualization Everywhere
 * Copyright (C) 2011-2015  ZeroTier, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * ZeroTier may be used and distributed under the terms of the GPLv3, which
 * are available at: http://www.gnu.org/licenses/gpl-3.0.html
 *
 * If you would like to embed ZeroTier into a commercial application or
 * redistribute it in a modified binary form, please contact ZeroTier Networks
 * LLC. Start here: http://www.zerotier.com/
 */

#ifndef ZT_WIRE_HPP
#define ZT_WIRE_HPP

#include <stdio.h>
#include <stdlib.h>

#ifdef __WINDOWS__
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#else
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif

#include <list>

#if defined(_WIN32) || defined(_WIN64)
#define ZT_WIRE_SOCKFD_TYPE SOCKET
#define ZT_WIRE_SOCKFD_NULL (INVALID_SOCKET)
#define ZT_WIRE_SOCKFD_VALID(s) ((s) != INVALID_SOCKET)
#define ZT_WIRE_CLOSE_SOCKET(s) ::closesocket(s)
#define ZT_WIRE_MAX_SOCKETS (FD_SETSIZE)
#define ZT_WIRE_SOCKADDR_STORAGE_TYPE struct sockaddr_storage
#else
#define ZT_WIRE_SOCKFD_TYPE int
#define ZT_WIRE_SOCKFD_NULL (-1)
#define ZT_WIRE_SOCKFD_VALID(s) ((s) > -1)
#define ZT_WIRE_CLOSE_SOCKET(s) ::close(s)
#define ZT_WIRE_MAX_SOCKETS (FD_SETSIZE)
#define ZT_WIRE_SOCKADDR_STORAGE_TYPE struct sockaddr_storage
#endif

namespace ZeroTier {

/**
 * Opaque socket type
 */
typedef const void * WireSocket;

/**
 * Simple templated non-blocking sockets implementation
 *
 * Yes there is boost::asio and libuv, but I like small binaries and I hate
 * build dependencies.
 *
 * This implementation takes four functions or function objects as template
 * paramters:
 *
 * ON_DATAGRAM_FUNCTION(WireSocket *sock,void **uptr,const struct sockaddr *from,void *data,unsigned long len)
 * ON_TCP_CONNECT_FUNCTION(WireSocket *sock,void **uptr,bool success)
 * ON_TCP_ACCEPT_FUNCTION(WireSocket *sockL,WireSocket *sockN,void **uptrL,void **uptrN,const struct sockaddr *from)
 * ON_TCP_CLOSE_FUNCTION(WireSocket *sock,void **uptr)
 * ON_TCP_DATA_FUNCTION(WireSocket *sock,void **uptr,void *data,unsigned long len)
 * ON_TCP_WRITABLE_FUNCTION(WireSocket *sock,void **uptr)
 *
 * These templates typically refer to function objects. Templates are used to
 * avoid the call overhead of indirection, which is surprisingly high for high
 * bandwidth applications pushing a lot of packets.
 *
 * The 'sock' pointer above is an opaque pointer to a socket. Each socket
 * has a 'uptr' user-settable/modifiable pointer associated with it, which
 * can be set on bind/connect calls and is passed as a void ** to permit
 * resetting at any time. The ACCEPT handler takes two sets of sock and
 * uptr: sockL and uptrL for the listen socket, and sockN and uptrN for
 * the new TCP connection socket that has just been created.
 *
 * Handlers are always called. On outgoing TCP connection, CONNECT is always
 * called on either success or failure followed by DATA and/or WRITABLE as
 * indicated. On socket close, handlers are called unless close() is told
 * explicitly not to call handlers. It is safe to close a socket within a
 * handler, and in that case close() can be told not to call handlers to
 * prevent recursion.
 *
 * This isn't thread-safe with the exception of whack(), which is safe to
 * call from another thread to abort poll().
 */
template
<
	typename ON_DATAGRAM_FUNCTION,
	typename ON_TCP_CONNECT_FUNCTION,
	typename ON_TCP_ACCEPT_FUNCTION,
	typename ON_TCP_CLOSE_FUNCTION,
	typename ON_TCP_DATA_FUNCTION,
	typename ON_TCP_WRITABLE_FUNCTION
>
class Wire
{
public:
	/**
	 * @param dgHandler Function or function object to handle UDP or RAW datagrams
	 * @param tcpConnectHandler Handler for outgoing TCP connection attempts (success or failure)
	 * @param tcpAcceptHandler Handler for incoming TCP connections
	 * @param tcpDataHandler Handler for incoming TCP data
	 * @param tcpWritableHandler Handler to be called when TCP sockets are writable (if notification is on)
	 * @param noDelay If true, disable Nagle algorithm on new TCP sockets
	 */
	Wire(
		ON_DATAGRAM_FUNCTION dgHandler,
		ON_TCP_CONNECT_FUNCTION tcpConnectHandler,
		ON_TCP_ACCEPT_FUNCTION tcpAcceptHandler,
		ON_TCP_CLOSE_FUNCTION tcpCloseHandler,
		ON_TCP_DATA_FUNCTION tcpDataHandler,
		ON_TCP_WRITABLE_FUNCTION tcpWritableHandler,
		bool noDelay) :
		_dgHandler(dgHandler),
		_tcpConnectHandler(tcpConnectHandler),
		_tcpAcceptHandler(tcpAcceptHandler),
		_tcpCloseHandler(tcpCloseHandler),
		_tcpDataHandler(tcpDataHandler),
		_tcpWritableHandler(tcpWritableHandler)
	{
		FD_ZERO(&_readfds);
		FD_ZERO(&_writefds);
		FD_ZERO(&_exceptfds);

#if defined(_WIN32) || defined(_WIN64)
		SOCKET pipes[2];
		this->_winPipeHack(pipes);
#else
		int pipes[2];
		if (::pipe(pipes))
			throw std::runtime_error("unable to create pipes for select() abort");
#endif

		_nfds = (pipes[0] > pipes[1]) ? (long)pipes[0] : (long)pipes[1];
		_whackReceiveSocket = pipes[0];
		_whackSendSocket = pipes[1];
		_noDelay = noDelay;
	}

	~Wire()
	{
		while (!_socks.empty())
			this->close((WireSocket *)&(_socks.front()),true);
		ZT_WIRE_CLOSE_SOCKET(_whackReceiveSocket);
		ZT_WIRE_CLOSE_SOCKET(_whackSendSocket);
	}

	/**
	 * Cause poll() to stop waiting immediately
	 */
	inline void whack()
	{
#if defined(_WIN32) || defined(_WIN64)
		::send(_whackSendSocket,(const char *)this,1,0);
#else
		::write(_whackSendSocket,(WireSocket *)this,1);
#endif
	}

	/**
	 * @return Number of open sockets
	 */
	inline unsigned long count() const throw() { return _socks.size(); }

	/**
	 * @return Maximum number of sockets allowed
	 */
	inline unsigned long maxCount() const throw() { return ZT_WIRE_MAX_SOCKETS; }

	/**
	 * Bind a UDP socket
	 *
	 * @param localAddress Local endpoint address and port
	 * @param uptr Initial value of user pointer associated with this socket
	 * @param bufferSize Desired socket receive/send buffer size -- will set as close to this as possible (0 to accept default)
	 * @return Socket or NULL on failure to bind
	 */
	inline WireSocket *udpBind(const struct sockaddr *localAddress,void *uptr,int bufferSize)
	{
		if (_socks.size() >= ZT_WIRE_MAX_SOCKETS)
			return (WireSocket *)0;

		ZT_WIRE_SOCKFD_TYPE s = ::socket(localAddress->sa_family,SOCK_DGRAM,0);
		if (!ZT_WIRE_SOCKFD_VALID(s))
			return (WireSocket *)0;

		if (bufferSize > 0) {
			int bs = bufferSize;
			while (bs >= 65536) {
				int tmpbs = bs;
				if (setsockopt(s,SOL_SOCKET,SO_RCVBUF,(const char *)&tmpbs,sizeof(tmpbs)) == 0)
					break;
				bs -= 16384;
			}
			bs = bufferSize;
			while (bs >= 65536) {
				int tmpbs = bs;
				if (setsockopt(s,SOL_SOCKET,SO_SNDBUF,(const char *)&tmpbs,sizeof(tmpbs)) == 0)
					break;
				bs -= 16384;
			}
		}

#if defined(_WIN32) || defined(_WIN64)
		{
			BOOL f;
			if (localAddress->sa_family == AF_INET6) {
				f = TRUE; setsockopt(s,IPPROTO_IPV6,IPV6_V6ONLY,(const char *)&f,sizeof(f));
				f = FALSE; setsockopt(s,IPPROTO_IPV6,IPV6_DONTFRAG,(const char *)&f,sizeof(f));
			}
			f = FALSE; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,(const char *)&f,sizeof(f));
			f = TRUE; setsockopt(s,SOL_SOCKET,SO_BROADCAST,(const char *)&f,sizeof(f));
		}
#else // not Windows
		{
			int f;
			if (localAddress->sa_family == AF_INET6) {
				f = 1; setsockopt(s,IPPROTO_IPV6,IPV6_V6ONLY,(void *)&f,sizeof(f));
#ifdef IPV6_MTU_DISCOVER
				f = 0; setsockopt(s,IPPROTO_IPV6,IPV6_MTU_DISCOVER,&f,sizeof(f));
#endif
			}
			f = 0; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,(void *)&f,sizeof(f));
			f = 1; setsockopt(s,SOL_SOCKET,SO_BROADCAST,(void *)&f,sizeof(f));
#ifdef IP_DONTFRAG
			f = 0; setsockopt(s,IPPROTO_IP,IP_DONTFRAG,&f,sizeof(f));
#endif
#ifdef IP_MTU_DISCOVER
			f = 0; setsockopt(s,IPPROTO_IP,IP_MTU_DISCOVER,&f,sizeof(f));
#endif
		}
#endif // Windows or not

		if (::bind(s,localAddress,(localAddress->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in))) {
			ZT_WIRE_CLOSE_SOCKET(s);
			return (WireSocket *)0;
		}

#if defined(_WIN32) || defined(_WIN64)
		{ u_long iMode=1; ioctlsocket(s,FIONBIO,&iMode); }
#else
		fcntl(s,F_SETFL,O_NONBLOCK);
#endif

		try {
			_socks.push_back(WireSocketImpl());
		} catch ( ... ) {
			ZT_WIRE_CLOSE_SOCKET(s);
			return (WireSocket *)0;
		}
		WireSocketImpl &sws = _socks.back();

		if ((long)s > _nfds)
			_nfds = (long)s;
		FD_SET(s,&_readfds);
		sws.type = ZT_WIRE_SOCKET_UDP;
		sws.sock = s;
		sws.uptr = uptr;
		memcpy(&(sws.saddr),localAddress,sizeof(struct sockaddr_storage));

		return (WireSocket *)&sws;
	}

	/**
	 * Send a UDP packet
	 *
	 * @param sock UDP socket
	 * @param addr Destination address (must be correct type for socket)
	 * @param addrlen Length of sockaddr_X structure
	 * @param data Data to send
	 * @param len Length of packet
	 * @return True if packet appears to have been sent successfully
	 */
	inline bool udpSend(WireSocket *sock,const struct sockaddr *addr,unsigned int addrlen,WireSocket *data,unsigned long len)
	{
		WireSocketImpl &sws = *(const_cast <WireSocketImpl *>(reinterpret_cast<const WireSocketImpl *>(sock)));
		return ((long)::sendto(sws.sock,data,len,0,addr,(socklen_t)addrlen) == (long)len);
	}

	/**
	 * Bind a local listen socket to listen for new TCP connections
	 *
	 * @param localAddress Local address and port
	 * @param uptr Initial value of uptr for new socket
	 * @return Socket or NULL on failure to bind
	 */
	inline WireSocket *tcpListen(const struct sockaddr *localAddress,void *uptr)
	{
		if (_socks.size() >= ZT_WIRE_MAX_SOCKETS)
			return (WireSocket *)0;
	}

	/**
	 * Start a non-blocking connect; CONNECT handler is called on success or failure
	 *
	 * Note that if NULL is returned here, the handler is not called. Such
	 * a return would indicate failure to allocate the socket, too many
	 * open sockets, etc.
	 *
	 * @param remoteAddress Remote address
	 * @param uptr Initial value of uptr for new socket
	 * @return New socket or NULL on failure
	 */
	inline WireSocket *tcpConnect(const struct sockaddr *remoteAddress,void *uptr)
	{
		if (_socks.size() >= ZT_WIRE_MAX_SOCKETS)
			return (WireSocket *)0;
	}

	/**
	 * Attempt to send data to a TCP connection (non-blocking)
	 *
	 * @param sock An open TCP socket (other socket types will fail)
	 * @param data Data to send
	 * @param len Length of data
	 * @return Number of bytes actually sent or 0 on failure
	 */
	inline unsigned long tcpSend(WireSocket *sock,WireSocket *data,unsigned long len)
	{
		WireSocketImpl &sws = *(const_cast <WireSocketImpl *>(reinterpret_cast<const WireSocketImpl *>(sock)));
		long n = ::send(sws.sock,data,len,0);
		return ((n > 0) ? (unsigned long)n : 0);
	}

	/**
	 * Set whether we want to be notified via the TCP writability handler when a socket is writable
	 *
	 * Call whack() if this is being done from another thread and you want
	 * it to take effect immediately. Otherwise it is only guaranteed to
	 * take effect on the next poll().
	 *
	 * @param sock TCP connection socket (other types are not valid)
	 * @param notifyWritable Want writable notifications?
	 */
	inline const void tcpSetNotifyWritable(WireSocket *sock,bool notifyWritable)
	{
		WireSocketImpl &sws = *(const_cast <WireSocketImpl *>(reinterpret_cast<const WireSocketImpl *>(sock)));
		if (notifyWritable) {
			FD_SET(sws.sock,&_writefds);
		} else {
			FD_CLR(sws.sock,&_writefds);
		}
	}

	/**
	 * Wait for activity and handle one or more events
	 *
	 * Note that this is not guaranteed to wait up to 'timeout' even
	 * if nothing happens, as whack() or other events such as signals
	 * may cause premature termination.
	 *
	 * @param timeout Timeout in milliseconds or 0 for none (forever)
	 */
	inline void poll(unsigned long timeout)
	{
		char buf[131072];
		struct sockaddr_storage ss;
		struct timeval tv;
		fd_set rfds,wfds,efds;

		memcpy(&rfds,&_readfds,sizeof(rfds));
		memcpy(&wfds,&_writefds,sizeof(wfds));
		memcpy(&efds,&_exceptfds,sizeof(efds));

		tv.tv_sec = (long)(timeout / 1000);
		tv.tv_usec = (long)((timeout % 1000) * 1000);
		select((int)_nfds + 1,&rfds,&wfds,&efds,(timeout > 0) ? &tv : (struct timeval *)0);

		if (FD_ISSET(_whackReceiveSocket,&rfds)) {
			char tmp[16];
#if defined(_WIN32) || defined(_WIN64)
			::recv(_whackReceiveSocket,tmp,16,0);
#else
			::read(_whackReceiveSocket,tmp,16);
#endif
		}

		for(std::list<WireSocketImpl>::iterator s(_socks.begin());s!=_socks.end();++s) {
			switch (s->type) {
				case ZT_WIRE_SOCKET_TCP_OUT_PENDING:
					if (FD_ISSET(s->sock,&efds))
						this->close((WireSocket *)&(_socks[i]),true);
					else if (FD_ISSET(s->sock,&wfds)) {
						socklen_t slen = sizeof(ss);
						if (::getpeername(s->sock,(strut sockaddr *)&ss,&slen) != 0)
							this->close((WireSocket *)&(_socks[i]),true);
						else {
							s->type = ZT_WIRE_SOCKET_TCP_OUT_CONNECTED;
							FD_SET(s->sock,&_readfds);
							FD_CLR(s->sock,&_writefds);
							FD_CLR(s->sock,&_exceptfds);
							try {
								_tcpConnectHandler((WireSocket *)&(_socks[i]),&(s->uptr),true);
							} catch ( ... ) {}
						}
					}
					break;
				case ZT_WIRE_SOCKET_TCP_OUT_CONNECTED:
				case ZT_WIRE_SOCKET_TCP_IN:
					if (FD_ISSET(s->sock,&rfds)) {
						long n = (long)::recv(s->sock,buf,sizeof(buf),0);
						if (n <= 0) {
							this->close((WireSocket *)&(_socks[i]),true);
						} else {
							try {
								_tcpDataHandler((WireSocket *)&(_socks[i]),&(s->uptr),(void *)buf,(unsigned long)n);
							} catch ( ... ) {}
						}
					}
					if ((FD_ISSET(s->sock,&wfds))&&(FD_ISSET(s->sock,&_writefds))) {
						try {
							_tcpWritableHandler((WireSocket *)&(_socks[i]),&(s->uptr));
						} catch ( ... ) {}
					}
					break;
				case ZT_WIRE_SOCKET_TCP_LISTEN:
					if (FD_ISSET(s->sock,&rfds)) {
						memset(&ss,0,sizeof(ss));
						socklen_t slen = sizeof(ss);
						ZT_WIRE_SOCKFD_TYPE newSock = ::accept(s->sock,(struct sockaddr *)&ss,&slen);
						if (ZT_WIRE_SOCKFD_VALID(newSock)) {
							if (_socks.size() >= ZT_WIRE_MAX_SOCKETS) {
								ZT_WIRE_CLOSE_SOCKET(newSock);
							} else {
#if defined(_WIN32) || defined(_WIN64)
								{ BOOL f = (_noDelay ? TRUE : FALSE); setsockopt(newSock,IPPROTO_TCP,TCP_NODELAY,(char *)&f,sizeof(f)); }
								{ u_long iMode=1; ioctlsocket(newSock,FIONBIO,&iMode); }
#else
								{ int f = (_noDelay ? 1 : 0); setsockopt(newSock,IPPROTO_TCP,TCP_NODELAY,(char *)&f,sizeof(f)); }
								fcntl(newSock,F_SETFL,O_NONBLOCK);
#endif
								_socks.push_back(WireSocketImpl());
								WireSocketImpl &sws = _socks.back();
								FD_SET(newSock,&_readfds);
								if ((long)newSock > _nfds)
									_nfds = (long)newSock;
								sws.type = ZT_WIRE_SOCKET_UDP;
								sws.sock = s;
								sws.uptr = (void *)0;
								memcpy(&(sws.saddr),&ss,sizeof(struct sockaddr_storage));
								try {
									_tcpAcceptHandler((WireSocket *)&(*s),(WireSocket *)&(_socks.back()),&(s->uptr),&(sws.uptr),(const struct sockaddr *)&(sws.saddr));
								} catch ( ... ) {}
						}
					}
					break;
				case ZT_WIRE_SOCKET_UDP:
					if (FD_ISSET(s->sock,&rfds)) {
						memset(&ss,0,sizeof(ss));
						socklen_t slen = sizeof(ss);
						long n = (long)::recvfrom(s->sock,buf,sizeof(buf),0,(struct sockaddr *)&ss,&slen);
						if (n > 0) {
							try {
								_dgHandler((WireSocket *)&(_socks[i]),&(s->uptr),(const struct sockaddr *)&ss,(void *)buf,(unsigned long)n);
							} catch ( ... ) {}
						}
					}
					break;
				default:
					break;
			}
		}
	}

	inline void close(WireSocket *sock,bool callHandlers)
	{
		if (!sock)
			return;
		WireSocketImpl &sws = *(const_cast <WireSocketImpl *>(reinterpret_cast<const WireSocketImpl *>(sock)));

		FD_CLR(sws.sock,&_readfds);
		FD_CLR(sws.sock,&_writefds);
		FD_CLR(sws.sock,&_exceptfds);

		ZT_WIRE_CLOSE_SOCKET(sws.sock);

		switch(sws.type) {
			case ZT_WIRE_SOCKET_TCP_OUT_PENDING:
				if (callHandlers) {
					try {
						_tcpConnectHandler(sock,&(sws.uptr),false);
					} catch ( ... ) {}
				}
				break;
			case ZT_WIRE_SOCKET_TCP_OUT_CONNECTED:
			case ZT_WIRE_SOCKET_TCP_IN:
				if (callHandlers) {
					try {
						_tcpCloseHandler(sock,&(sws.uptr));
					} catch ( ... ) {}
				}
				break;
			default:
				break;
		}

		if ((long)sws.sock >= _nfds) {
			long nfds = (long)_whackSendSocket;
			if ((long)_whackReceiveSocket > nfds)
				nfds = (long)_whackReceiveSocket;
			for(std::list<WireSocketImpl>::iterator s(_socks.begin());s!=_socks.end();++s) {
				if ((long)s->sock > nfds)
					nfds = (long)s->sock;
			}
			_nfds = nfds;
		}

		for(std::list<WireSocketImpl>::iterator s(_socks.begin());s!=_socks.end();++s) {
			if (&(*s) == sock) {
				_socks.erase(s);
				break;
			}
		}
	}

private:
#if defined(_WIN32) || defined(_WIN64)
	// hack copied from StackOverflow, behaves a bit like pipe() on *nix systems
	inline void _winPipeHack(SOCKET fds[2]) const
	{
		struct sockaddr_in inaddr;
		struct sockaddr addr;
		SOCKET lst=::socket(AF_INET, SOCK_STREAM,IPPROTO_TCP);
		if (lst == INVALID_SOCKET)
			throw std::runtime_error("unable to create pipes for select() abort");
		memset(&inaddr, 0, sizeof(inaddr));
		memset(&addr, 0, sizeof(addr));
		inaddr.sin_family = AF_INET;
		inaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		inaddr.sin_port = 0;
		int yes=1;
		setsockopt(lst,SOL_SOCKET,SO_REUSEADDR,(char*)&yes,sizeof(yes));
		bind(lst,(struct sockaddr *)&inaddr,sizeof(inaddr));
		listen(lst,1);
		int len=sizeof(inaddr);
		getsockname(lst, &addr,&len);
		fds[0]=::socket(AF_INET, SOCK_STREAM,0);
		if (fds[0] == INVALID_SOCKET)
			throw std::runtime_error("unable to create pipes for select() abort");
		connect(fds[0],&addr,len);
		fds[1]=accept(lst,0,0);
		closesocket(lst);
	}
#endif

	enum WireSocketType
	{
		ZT_WIRE_SOCKET_TCP_OUT_PENDING = 0x00,
		ZT_WIRE_SOCKET_TCP_OUT_CONNECTED = 0x01,
		ZT_WIRE_SOCKET_TCP_IN = 0x02,
		ZT_WIRE_SOCKET_TCP_LISTEN = 0x03, // isTCP() == ((type & 0x03) != 0)
		ZT_WIRE_SOCKET_RAW = 0x04,
		ZT_WIRE_SOCKET_UDP = 0x05
	};

	struct WireSocketImpl
	{
		WireSocketType type;
		ZT_WIRE_SOCKFD_TYPE sock;
		void *uptr; // user-settable pointer
		ZT_WIRE_SOCKADDR_STORAGE_TYPE saddr; // from address for TCP_IN, local address otherwise
	};

	inline bool _isTCP(const WireSocketImpl &sws) const throw() { return ((((unsigned int)sws.type) & 0x03) != 0); }

	ON_DATAGRAM_FUNCTION _dgHandler;
	ON_TCP_CONNECT_FUNCTION _tcpConnectHandler;
	ON_TCP_ACCEPT_FUNCTION _tcpAcceptHandler;
	ON_TCP_CLOSE_FUNCTION _tcpCloseHandler;
	ON_TCP_DATA_FUNCTION _tcpDataHandler;
	ON_TCP_WRITABLE_FUNCTION _tcpWritableHandler;

	std::list<WireSocketImpl> _socks;
	fd_set _readfds,_writefds,_exceptfds;
	long _nfds;

	ZT_WIRE_SOCKFD_TYPE _whackReceiveSocket;
	ZT_WIRE_SOCKFD_TYPE _whackSendSocket;

	bool _noDelay;
};

} // namespace ZeroTier

#endif
