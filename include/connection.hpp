//-------------------------------------------------------------------------------------------------
/*

Fix8 is released under the GNU LESSER GENERAL PUBLIC LICENSE Version 3.

Fix8 Open Source FIX Engine.
Copyright (C) 2010-13 David L. Dight <fix@fix8.org>

Fix8 is free software: you can  redistribute it and / or modify  it under the  terms of the
GNU Lesser General  Public License as  published  by the Free  Software Foundation,  either
version 3 of the License, or (at your option) any later version.

Fix8 is distributed in the hope  that it will be useful, but WITHOUT ANY WARRANTY;  without
even the  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

You should  have received a copy of the GNU Lesser General Public  License along with Fix8.
If not, see <http://www.gnu.org/licenses/>.

BECAUSE THE PROGRAM IS  LICENSED FREE OF  CHARGE, THERE IS NO  WARRANTY FOR THE PROGRAM, TO
THE EXTENT  PERMITTED  BY  APPLICABLE  LAW.  EXCEPT WHEN  OTHERWISE  STATED IN  WRITING THE
COPYRIGHT HOLDERS AND/OR OTHER PARTIES  PROVIDE THE PROGRAM "AS IS" WITHOUT WARRANTY OF ANY
KIND,  EITHER EXPRESSED   OR   IMPLIED,  INCLUDING,  BUT   NOT  LIMITED   TO,  THE  IMPLIED
WARRANTIES  OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.  THE ENTIRE RISK AS TO
THE QUALITY AND PERFORMANCE OF THE PROGRAM IS WITH YOU. SHOULD THE PROGRAM PROVE DEFECTIVE,
YOU ASSUME THE COST OF ALL NECESSARY SERVICING, REPAIR OR CORRECTION.

IN NO EVENT UNLESS REQUIRED  BY APPLICABLE LAW  OR AGREED TO IN  WRITING WILL ANY COPYRIGHT
HOLDER, OR  ANY OTHER PARTY  WHO MAY MODIFY  AND/OR REDISTRIBUTE  THE PROGRAM AS  PERMITTED
ABOVE,  BE  LIABLE  TO  YOU  FOR  DAMAGES,  INCLUDING  ANY  GENERAL, SPECIAL, INCIDENTAL OR
CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OR INABILITY TO USE THE PROGRAM (INCLUDING BUT
NOT LIMITED TO LOSS OF DATA OR DATA BEING RENDERED INACCURATE OR LOSSES SUSTAINED BY YOU OR
THIRD PARTIES OR A FAILURE OF THE PROGRAM TO OPERATE WITH ANY OTHER PROGRAMS), EVEN IF SUCH
HOLDER OR OTHER PARTY HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.

*/
//-------------------------------------------------------------------------------------------------
#ifndef _FIX8_CONNECTION_HPP_
# define _FIX8_CONNECTION_HPP_

#include <Poco/Net/StreamSocket.h>
#include <Poco/Timespan.h>
#include <Poco/Net/NetException.h>

//----------------------------------------------------------------------------------------
namespace FIX8 {

class Session;

//----------------------------------------------------------------------------------------
/// Half duplex async socket wrapper with thread
/*! \tparam T the object type to queue */
template <typename T>
class AsyncSocket
{
	dthread<AsyncSocket> _thread;

protected:
	coroutine _coro;
	Poco::Net::StreamSocket *_sock;
	f8_concurrent_queue<T> _msg_queue;
	Session& _session;
	ProcessModel _pmodel;

public:
	/*! Ctor.
	    \param sock connected socket
	    \param session session
	    \param pmodel process model */
	AsyncSocket(Poco::Net::StreamSocket *sock, Session& session, const ProcessModel pmodel=pm_pipeline)
		: _thread(FIX8::ref(*this)), _sock(sock), _session(session), _pmodel(pmodel) {}

	/// Dtor.
	virtual ~AsyncSocket() {}

	/*! Get the number of messages queued on this socket.
	    \return number of queued messages */
	size_t queued() const { return _msg_queue.size(); }

	/*! Function operator. Called by thread to process message on queue.
	    \return 0 on success */
	virtual int operator()() { return 0; }

	/*! Execute the function operator
	    \return result of operator */
	virtual int execute() { return (*this)(); }

	/// Start the processing thread.
	virtual void start() { _thread.start(); }

	/// Stop the processing thread and quit.
	virtual void quit() { _thread.kill(1); }

	/*! Get the underlying socket object.
	    \return the socket */
	Poco::Net::StreamSocket *socket() { return _sock; }

	/*! Wait till processing thead has finished.
	    \return 0 on success */
	int join() { return _thread.join(); }
};

//----------------------------------------------------------------------------------------
/// Fix message reader
class FIXReader : public AsyncSocket<f8String>
{
	enum { _max_msg_len = MAX_MSG_LENGTH, _chksum_sz = 7 };
	f8_atomic<bool> _socket_error;

	dthread<FIXReader> _callback_thread;

	/*! Process messages from inbound queue, calls session process method.
	    \return number of messages processed */
	int callback_processor();

	size_t _bg_sz; // 8=FIXx.x^A9=x

	/*! Read a Fix message. Throws InvalidBodyLength, IllegalMessage.
	    \param to string to place message in
	    \return true on success */
	bool read(f8String& to);

	/*! Read bytes from the socket layer, throws PeerResetConnection.
	    \param where buffer to place bytes in
	    \param sz number of bytes to read
	    \return number of bytes read */
	int sockRead(char *where, const size_t sz)
	{
		unsigned remaining(sz), rddone(0);

		while (remaining > 0)
		{
			const int rdSz(_sock->receiveBytes(where + rddone, remaining));
			if (rdSz <= 0)
			{
				if (errno == EAGAIN
#if defined EWOULDBLOCK && EAGAIN != EWOULDBLOCK
					|| errno == EWOULDBLOCK
#endif
				)
					continue;
				throw PeerResetConnection("sockRead: connection gone");
			}

			rddone += rdSz;
			remaining -= rdSz;
		}

		return rddone;
	}

protected:
	/*! Reader thread method. Reads messages and places them on the queue for processing.
	  Supports pipelined, threaded and coroutine process models.
	    \return 0 on success */
	int operator()();

public:
	/*! Ctor.
	    \param sock connected socket
	    \param session session
	    \param pmodel process model */
	FIXReader(Poco::Net::StreamSocket *sock, Session& session, const ProcessModel pmodel=pm_pipeline)
		: AsyncSocket<f8String>(sock, session, pmodel), _callback_thread(FIX8::ref(*this), &FIXReader::callback_processor), _bg_sz()
	{
		set_preamble_sz();
	}

	/// Dtor.
	virtual ~FIXReader() {}

	/// Start the processing threads.
	virtual void start()
	{
		_socket_error = false;
		if (_pmodel != pm_coro)
			AsyncSocket<f8String>::start();
		if (_pmodel == pm_pipeline)
		{
			if (_callback_thread.start())
				_socket_error = true;
		}
	}

	/// Stop the processing threads and quit.
	virtual void quit()
	{
		if (_pmodel == pm_pipeline)
			_callback_thread.kill(1);
		AsyncSocket<f8String>::quit();
	}

	/// Send a message to the processing method instructing it to quit.
	virtual void stop()
	{
		if (_pmodel == pm_pipeline)
		{
			const f8String from;
			_msg_queue.try_push(from);
		}
	}

	/*! Wait till writer thead has finished.
	    \return 0 on success */
   int join() { return _pmodel != pm_coro ? AsyncSocket<f8String>::join() : -1; }

	/// Calculate the length of the Fix message preamble, e.g. "8=FIX.4.4^A9=".
	void set_preamble_sz();

	/*! Check to see if the socket is in error
	    \return true if there was a socket error */
	bool is_socket_error() const { return _socket_error; }

	/*! Check to see if there is any data waiting to be read
	    \return true of data ready */
	bool poll() const
	{
		static const Poco::Timespan ts;
		return _sock->poll(ts, Poco::Net::Socket::SELECT_READ);
	}
};

//----------------------------------------------------------------------------------------
/// Fix message writer
class FIXWriter : public AsyncSocket<Message *>
{
protected:
	/*! Writer thread method. Reads messages from the queue and sends them over the socket.
	    \return 0 on success */
	int operator()();

public:
	/*! Ctor.
	    \param sock connected socket
	    \param session session
	    \param pmodel process model */
	FIXWriter(Poco::Net::StreamSocket *sock, Session& session, const ProcessModel pmodel=pm_pipeline)
		: AsyncSocket<Message *>(sock, session, pmodel) {}

	/// Dtor.
	virtual ~FIXWriter() {}

	/*! Place Fix message on outbound message queue.
	    \param from message to send
	    \return true in success */
	bool write(Message *from)
	{
		if (_pmodel == pm_pipeline)
			return _msg_queue.try_push(from);
#if defined MSGRECYCLING
		const bool result(_session.send_process(from));
		from->set_in_use(false);
#else
		scoped_ptr<Message> msg(from);
		const bool result(_session.send_process(msg.get()));
#endif
		return result;
	}

	/*! Wait till writer thead has finished.
	    \return 0 on success */
   int join() { return _pmodel == pm_pipeline ? AsyncSocket<Message *>::join() : -1; }

	/*! Send Fix message directly
	    \param from message to send
	    \return true in success */
	bool write(Message& from)
	{
		if (_pmodel == pm_pipeline) // not permitted if pipeling
			throw f8Exception("cannot send message directly if pipelining");
#if defined MSGRECYCLING
		const bool result(_session.send_process(&from));
		from.set_in_use(false);
		return result;
#else
		return _session.send_process(&from);
#endif
	}

	/*! Check to see if a write would block
	    \return true if a write would block */
	bool poll() const
	{
		static const Poco::Timespan ts;
		return _sock->poll(ts, Poco::Net::Socket::SELECT_WRITE);
	}

	/*! Send message over socket.
	    \param data char * buffer to send
	    \param remaining number of bytes
	    \return number of bytes sent */
	int send(const char *data, size_t remaining)
	{
		unsigned wrdone(0);

		while (remaining > 0)
		{
			const int wrtSz(_sock->sendBytes(data + wrdone, remaining));
			if (wrtSz < 0)
			{
				if (errno == EAGAIN
#if defined EWOULDBLOCK && EAGAIN != EWOULDBLOCK
					|| errno == EWOULDBLOCK
#endif
				)
					continue;
				throw PeerResetConnection("send: connection gone");
			}

			wrdone += wrtSz;
			remaining -= wrtSz;
		}

		return wrdone;
	}

	/// Start the processing threads.
	virtual void start()
	{
		if (_pmodel == pm_pipeline)
			AsyncSocket<Message *>::start();
	}

	/// Send a message to the processing method instructing it to quit.
	virtual void stop() { _msg_queue.try_push(0); }
};

//----------------------------------------------------------------------------------------
/// Complete Fix connection (reader and writer).
class Connection
{
public:
	/// Roles: acceptor, initiator or unknown.
	enum Role { cn_acceptor, cn_initiator, cn_unknown };

protected:
	Poco::Net::StreamSocket *_sock;
	Poco::Net::SocketAddress _addr;
	bool _connected;
	Session& _session;
	Role _role;
	ProcessModel _pmodel;
	unsigned _hb_interval, _hb_interval20pc;

	FIXReader _reader;
	FIXWriter _writer;

public:
	/*! Ctor. Initiator.
	    \param sock connected socket
	    \param addr sock address structure
	    \param session session
	    \param pmodel process model
	    \param hb_interval heartbeat interval */
	Connection(Poco::Net::StreamSocket *sock, Poco::Net::SocketAddress& addr, Session &session, // client
        const ProcessModel pmodel, const unsigned hb_interval)
		: _sock(sock), _addr(addr), _connected(), _session(session), _role(cn_initiator), _pmodel(pmodel),
        _hb_interval(hb_interval), _reader(sock, session, pmodel), _writer(sock, session, pmodel) {}

	/*! Ctor. Acceptor.
	    \param sock connected socket
	    \param addr sock address structure
	    \param session session
	    \param hb_interval heartbeat interval
	    \param pmodel process model */
	Connection(Poco::Net::StreamSocket *sock, Poco::Net::SocketAddress& addr, Session &session, // server
		const unsigned hb_interval, const ProcessModel pmodel)
		: _sock(sock), _addr(addr), _connected(true), _session(session), _role(cn_acceptor), _pmodel(pmodel),
		_hb_interval(hb_interval), _hb_interval20pc(hb_interval + hb_interval / 5),
		  _reader(sock, session, pmodel), _writer(sock, session, pmodel) {}

	/// Dtor.
	virtual ~Connection() {}

	/*! Get the role for this connection.
	    \return the role */
	Role get_role() const { return _role; }

	/*! Get the process model
	  \return the process model */
	ProcessModel get_pmodel() const { return _pmodel; }

	/// Start the reader and writer threads.
	void start();

	/// Stop the reader and writer threads.
	void stop();

	/*! Get the connection state.
	    \return true if connected */
	virtual bool connect() { return _connected; }

	/*! Write a message to the underlying socket.
	    \param from Message to write
	    \return true on success */
	virtual bool write(Message *from) { return _writer.write(from); }

	/*! Write a message to the underlying socket. Non-pipelined version.
	    \param from Message to write
	    \return true on success */
	virtual bool write(Message& from) { return _writer.write(from); }

	/*! Write a string message to the underlying socket.
	    \param from Message (string) to write
	    \param sz number bytes to send
	    \return number of bytes written */
	int send(const char *from, size_t sz) { return _writer.send(from, sz); }

	/*! Write a string message to the underlying socket.
	    \param from Message (string) to write
	    \return number of bytes written */
	int send(const f8String& from) { return _writer.send(from.data(), from.size()); }

	/*! Set the heartbeat interval for this connection.
	    \param hb_interval heartbeat interval */
	void set_hb_interval(const unsigned hb_interval)
		{ _hb_interval = hb_interval; _hb_interval20pc = hb_interval + hb_interval / 5; }

	/*! Get the heartbeat interval for this connection.
	    \return the heartbeat interval */
	unsigned get_hb_interval() const { return _hb_interval; }

	/*! Get the heartbeat interval + %20 for this connection.
	    \return the heartbeat interval + %20 */
	unsigned get_hb_interval20pc() const { return _hb_interval20pc; }

	/*! Get the peer socket address
	    \return peer socket address reference */
	const Poco::Net::SocketAddress get_peer_socket_address() const { return _sock->peerAddress(); }

	/*! Get the socket address
	    \return socket address reference */
	const Poco::Net::SocketAddress& get_socket_address() const { return _addr; }

	/*! Wait till reader thead has finished.
	    \return 0 on success */
	int join() { return _reader.join(); }

	/*! Check to see if the socket is in error
	    \return true if there was a socket error */
	bool is_socket_error() const { return _reader.is_socket_error(); }

	/*! Set the socket recv buffer sz
	    \param sock socket to operate on
	    \param sz new size */
	static void set_recv_buf_sz(const unsigned sz, Poco::Net::Socket *sock)
	{
		const unsigned current_sz(sock->getReceiveBufferSize());
		sock->setReceiveBufferSize(sz);
		std::ostringstream ostr;
		ostr << "ReceiveBufferSize old:" << current_sz << " requested:" << sz << " new:" << sock->getReceiveBufferSize();
		GlobalLogger::log(ostr.str());
	}

	/*! Set the socket send buffer sz
	    \param sock socket to operate on
	    \param sz new size */
	static void set_send_buf_sz(const unsigned sz, Poco::Net::Socket *sock)
	{
		const unsigned current_sz(sock->getSendBufferSize());
		sock->setSendBufferSize(sz);
		std::ostringstream ostr;
		ostr << "SendBufferSize old:" << current_sz << " requested:" << sz << " new:" << sock->getSendBufferSize();
		GlobalLogger::log(ostr.str());
	}
	/*! Set the socket recv buffer sz
	    \param sz new size */
	void set_recv_buf_sz(const unsigned sz) const { set_recv_buf_sz(sz, _sock); }

	/*! Set the socket send buffer sz
	    \param sz new size */
	void set_send_buf_sz(const unsigned sz) const { set_send_buf_sz(sz, _sock); }

	/*! Get the session associated with this connection.
	    \return the session */
	Session& get_session() { return _session; }

	/*! Call the FIXreader method
	    \return result of call */
	int reader_execute() { return _reader.execute(); }

	/*! Check if the reader will block
	    \return true if won't block */
	bool reader_poll() const { return _reader.poll(); }

	/*! Call the FIXreader method
	    \return result of call */
	int writer_execute() { return _writer.execute(); }

	/*! Check if the writer will block
	    \return true if won't block */
	bool writer_poll() const { return _writer.poll(); }
};

//-------------------------------------------------------------------------------------------------
/// Client (initiator) specialisation of Connection.
class ClientConnection : public Connection
{
	const bool _no_delay;

public:
	/*! Ctor. Initiator.
	    \param sock connected socket
	    \param addr sock address structure
	    \param session session
	    \param hb_interval heartbeat interval
	    \param pmodel process model
	    \param no_delay set or clear the tcp no delay flag on the socket */
    ClientConnection(Poco::Net::StreamSocket *sock, Poco::Net::SocketAddress& addr,
            Session &session, const unsigned hb_interval, const ProcessModel pmodel=pm_pipeline, const bool no_delay=true)
        : Connection(sock, addr, session, pmodel, hb_interval), _no_delay(no_delay) {}

	/// Dtor.
	virtual ~ClientConnection() {}

	/*! Establish connection.
	    \return true on success */
	bool connect();
};

//-------------------------------------------------------------------------------------------------
/// Server (acceptor) specialisation of Connection.
class ServerConnection : public Connection
{
public:
	/*! Ctor. Initiator.
	    \param sock connected socket
	    \param addr sock address structure
	    \param session session
	    \param hb_interval heartbeat interval
	    \param pmodel process model
	    \param no_delay set or clear the tcp no delay flag on the socket */
	ServerConnection(Poco::Net::StreamSocket *sock, Poco::Net::SocketAddress& addr,
			Session &session, const unsigned hb_interval, const ProcessModel pmodel=pm_pipeline, const bool no_delay=true) :
		Connection(sock, addr, session, hb_interval, pmodel)
	{
		_sock->setLinger(false, 0);
		_sock->setNoDelay(no_delay);
	}

	/// Dtor.
	virtual ~ServerConnection() {}
};

//-------------------------------------------------------------------------------------------------

} // FIX8

#endif // _FIX8_CONNECTION_HPP_
