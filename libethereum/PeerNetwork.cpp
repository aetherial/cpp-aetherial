/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	Foobar is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Foobar.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file PeerNetwork.cpp
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 */

#include "Common.h"
#include "BlockChain.h"
#include "TransactionQueue.h"
#include "PeerNetwork.h"
using namespace std;
using namespace eth;

PeerSession::PeerSession(PeerServer* _s, bi::tcp::socket _socket, uint _rNId):
	m_server(_s),
	m_socket(std::move(_socket)),
	m_reqNetworkId(_rNId)
{
}

PeerSession::~PeerSession()
{
	disconnect();
}

bool PeerSession::interpret(RLP const& _r)
{
	switch (_r[0].toInt<unsigned>())
	{
	case Hello:
	{
		cout << std::setw(2) << m_socket.native_handle() << " | Hello" << endl;
		m_protocolVersion = _r[1].toInt<uint>();
		m_networkId = _r[2].toInt<uint>();
		auto clientVersion = _r[3].toString();
		if (m_protocolVersion != 0 || m_networkId != m_reqNetworkId)
		{
			disconnect();
			return false;
		}
		try
			{ m_info = PeerInfo({clientVersion, m_socket.remote_endpoint().address().to_string(), (short)m_socket.remote_endpoint().port(), std::chrono::steady_clock::duration()}); }
		catch (...)
		{
			disconnect();
			return false;
		}

		cout << std::setw(2) << m_socket.native_handle() << " | Client version: " << clientVersion << endl;

		// Grab their block chain off them.
		{
			RLPStream s;
			prep(s).appendList(3);
			s << (uint)GetChain;
			unsigned count = std::min<unsigned>(256, m_server->m_chain->details(m_server->m_latestBlockSent).number);
			s.appendList(count);
			auto h = m_server->m_chain->details(m_server->m_latestBlockSent).parent;
			for (unsigned i = 0; i < count; ++i, h = m_server->m_chain->details(h).parent)
				s << h;
			s << 256;
			sealAndSend(s);
		}
		break;
	}
	case Disconnect:
		m_socket.close();
		return false;
	case Ping:
	{
//		cout << std::setw(2) << m_socket.native_handle() << " | Ping" << endl;
		RLPStream s;
		sealAndSend(prep(s).appendList(1) << (uint)Pong);
		break;
	}
	case Pong:
		m_info.lastPing = std::chrono::steady_clock::now() - m_ping;
//		cout << "Latency: " << chrono::duration_cast<chrono::milliseconds>(m_lastPing).count() << " ms" << endl;
		break;
	case GetPeers:
	{
		cout << std::setw(2) << m_socket.native_handle() << " | GetPeers" << endl;
		std::vector<bi::tcp::endpoint> peers = m_server->potentialPeers();
		RLPStream s;
		prep(s).appendList(2);
		s << (uint)Peers;
		s.appendList(peers.size());
		for (auto i: peers)
			s.appendList(2) << i.address().to_v4().to_bytes() << i.port();
		sealAndSend(s);
		break;
	}
	case Peers:
		cout << std::setw(2) << m_socket.native_handle() << " | Peers (" << dec << _r[1].itemCount() << " entries)" << endl;
		for (auto i: _r[1])
		{
			auto ep = bi::tcp::endpoint(bi::address_v4(i[0].toArray<byte, 4>()), i[1].toInt<short>());
			m_server->m_incomingPeers.push_back(ep);
			cout << "New peer: " << ep << endl;
		}
		break;
	case Transactions:
		cout << std::setw(2) << m_socket.native_handle() << " | Transactions (" << dec << _r[1].itemCount() << " entries)" << endl;
		for (auto i: _r[1])
		{
			m_server->m_incomingTransactions.push_back(i.data().toBytes());
			m_knownTransactions.insert(sha3(i.data()));
		}
		break;
	case Blocks:
		cout << std::setw(2) << m_socket.native_handle() << " | Blocks (" << dec << _r[1].itemCount() << " entries)" << endl;
		for (auto i: _r[1])
		{
			m_server->m_incomingBlocks.push_back(i.data().toBytes());
			m_knownBlocks.insert(sha3(i.data()));
		}
		if (_r[1].itemCount())	// we received some - check if there's any more
		{
			RLPStream s;
			prep(s).appendList(3);
			s << (uint)GetChain;
			s.appendList(1);
			s << sha3(_r[1][0].data());
			s << 256;
			sealAndSend(s);
		}
		break;
	case GetChain:
	{
		// ********************************************************************
		// NEEDS FULL REWRITE!

		h256s parents = _r[1].toVector<h256>();
		if (!parents.size())
			break;
		// return 2048 block max.
		uint baseCount = (uint)min<bigint>(_r[2].toInt<bigint>(), 256);
		cout << std::setw(2) << m_socket.native_handle() << " | GetChain (" << baseCount << " max, from " << parents.front() << " to " << parents.back() << ")" << endl;
		for (auto parent: parents)
		{
			auto h = m_server->m_chain->currentHash();
			h256 latest = m_server->m_chain->currentHash();
			uint latestNumber = 0;
			uint parentNumber = 0;
			RLPStream s;

			if (m_server->m_chain->details(parent))
			{
				latestNumber = m_server->m_chain->details(latest).number;
				parentNumber = m_server->m_chain->details(parent).number;
				uint count = min<uint>(latestNumber - parentNumber, baseCount);
				cout << "Requires " << dec << (latestNumber - parentNumber) << " blocks from " << latestNumber << " to " << parentNumber << endl;
				cout << latest << " - " << parent << endl;

				prep(s);
				s.appendList(2) << (uint)Blocks;
				s.appendList(count);
				uint endNumber = m_server->m_chain->details(parent).number;
				uint startNumber = endNumber + count;
				cout << "Sending " << dec << count << " blocks from " << startNumber << " to " << endNumber << endl;

				uint n = latestNumber;
				for (; n > startNumber; n--, h = m_server->m_chain->details(h).parent) {}
				for (uint i = 0; h != parent && n > endNumber && i < count; ++i, --n, h = m_server->m_chain->details(h).parent)
				{
//					cout << "   " << dec << i << " " << h << endl;
					s.appendRaw(m_server->m_chain->block(h));
				}
				cout << "Parent: " << h << endl;
			}
			else if (parent != parents.back())
				continue;

			if (h == parent)
			{
			}
			else
			{
				// not in the blockchain;
				if (parent == parents.back())
				{
					// out of parents...
					cout << std::setw(2) << m_socket.native_handle() << " | GetChain failed; not in chain" << endl;
					// No good - must have been on a different branch.
					s.clear();
					prep(s).appendList(2) << (uint)NotInChain << parents.back();
				}
				else
					// still some parents left - try them.
					continue;
			}
			// send the packet (either Blocks or NotInChain) & exit.
			sealAndSend(s);
			break;
			// ********************************************************************
		}
		break;
	}
	case NotInChain:
	{
		h256 noGood = _r[1].toHash<h256>();
		cout << std::setw(2) << m_socket.native_handle() << " | NotInChain (" << noGood << ")" << endl;
		if (noGood != m_server->m_chain->genesisHash())
		{
			RLPStream s;
			prep(s).appendList(3);
			s << (uint)GetChain;
			unsigned count = std::min<unsigned>(256, m_server->m_chain->details(noGood).number);
			s.appendList(count);
			auto h = m_server->m_chain->details(noGood).parent;
			for (unsigned i = 0; i < count; ++i, h = m_server->m_chain->details(h).parent)
				s << h;
			s << 2048;
			sealAndSend(s);
		}
		// else our peer obviously knows nothing if they're unable to give the descendents of the genesis!
		break;
	}
	default:
		break;
	}
	return true;
}

void PeerSession::ping()
{
	RLPStream s;
	sealAndSend(prep(s).appendList(1) << Ping);
	m_ping = std::chrono::steady_clock::now();
}

RLPStream& PeerSession::prep(RLPStream& _s)
{
	return _s.appendRaw(bytes(8, 0));
}

void PeerSession::seal(bytes& _b)
{
	_b[0] = 0x22;
	_b[1] = 0x40;
	_b[2] = 0x08;
	_b[3] = 0x91;
	uint32_t len = _b.size() - 8;
	_b[4] = len >> 24;
	_b[5] = len >> 16;
	_b[6] = len >> 8;
	_b[7] = len;
}

void PeerSession::sealAndSend(RLPStream& _s)
{
	bytes b;
	_s.swapOut(b);
	seal(b);
	sendDestroy(b);
}

void PeerSession::sendDestroy(bytes& _msg)
{
	std::shared_ptr<bytes> buffer = std::make_shared<bytes>();
	swap(*buffer, _msg);
	assert((*buffer)[0] == 0x22);
	cout << "Sending " << (buffer->size() - 8) << endl;
//	cout << "Sending " << RLP(bytesConstRef(buffer.get()).cropped(8)) << endl;
	ba::async_write(m_socket, ba::buffer(*buffer), [=](boost::system::error_code ec, std::size_t length)
	{
		if (ec)
			dropped();
		cout << length << " bytes written (EC: " << ec << ")" << endl;
	});
}

void PeerSession::send(bytesConstRef _msg)
{
	std::shared_ptr<bytes> buffer = std::make_shared<bytes>(_msg.toBytes());
	assert((*buffer)[0] == 0x22);
	cout << "Sending " << (_msg.size() - 8) << endl;// RLP(bytesConstRef(buffer.get()).cropped(8)) << endl;
	ba::async_write(m_socket, ba::buffer(*buffer), [=](boost::system::error_code ec, std::size_t length)
	{
		if (ec)
			dropped();
		cout << length << " bytes written (EC: " << ec << ")" << endl;
	});
}

void PeerSession::dropped()
{
	m_socket.close();
	for (auto i = m_server->m_peers.begin(); i != m_server->m_peers.end(); ++i)
		if (i->lock().get() == this)
		{
			m_server->m_peers.erase(i);
			break;
		}
}

void PeerSession::disconnect()
{
	RLPStream s;
	prep(s);
	s.appendList(1) << (uint)Disconnect;
	sealAndSend(s);
	sleep(1);
	m_socket.close();
}

void PeerSession::start()
{
	RLPStream s;
	prep(s);
	s.appendList(4) << (uint)Hello << (uint)0 << (uint)0 << m_server->m_clientVersion;
	sealAndSend(s);

	ping();

	doRead();
}

void PeerSession::doRead()
{
	auto self(shared_from_this());
	m_socket.async_read_some(boost::asio::buffer(m_data), [this, self](boost::system::error_code ec, std::size_t length)
	{
		if (ec)
			dropped();
		else
		{
			m_incoming.resize(m_incoming.size() + length);
			memcpy(m_incoming.data() + m_incoming.size() - length, m_data.data(), length);
			while (m_incoming.size() > 8)
			{
				if (m_incoming[0] != 0x22 || m_incoming[1] != 0x40 || m_incoming[2] != 0x08 || m_incoming[3] != 0x91)
				{
					cout << "*** Out of alignment: skipping: " << hex << showbase << (int)m_incoming[0] << endl;
					memmove(m_incoming.data(), m_incoming.data() + 1, m_incoming.size() - 1);
					m_incoming.resize(m_incoming.size() - 1);
				}
				else
				{
					uint32_t len = fromBigEndian<uint32_t>(bytesConstRef(m_incoming.data() + 4, 4));
					cout << "Received packet of " << len << " bytes" << endl;
					if (m_incoming.size() - 8 < len)
						break;

					// enough has come in.
					RLP r(bytesConstRef(m_incoming.data() + 8, len));
					if (!interpret(r))
						// error
						break;
					memmove(m_incoming.data(), m_incoming.data() + len + 8, m_incoming.size() - (len + 8));
					m_incoming.resize(m_incoming.size() - (len + 8));
				}
			}
			doRead();
		}
	});
}

PeerServer::PeerServer(std::string const& _clientVersion, BlockChain const& _ch, uint _networkId, short _port):
	m_clientVersion(_clientVersion),
	m_chain(&_ch),
	m_acceptor(m_ioService, bi::tcp::endpoint(bi::tcp::v4(), _port)),
	m_socket(m_ioService),
	m_requiredNetworkId(_networkId)
{
	doAccept();
}

PeerServer::PeerServer(std::string const& _clientVersion, uint _networkId):
	m_clientVersion(_clientVersion),
	m_acceptor(m_ioService, bi::tcp::endpoint(bi::tcp::v4(), 0)),
	m_socket(m_ioService),
	m_requiredNetworkId(_networkId)
{
}

PeerServer::~PeerServer()
{
	for (auto const& i: m_peers)
		if (auto p = i.lock())
			p->disconnect();
}

std::vector<bi::tcp::endpoint> PeerServer::potentialPeers()
{
	std::vector<bi::tcp::endpoint> ret;
	bool haveLocal = false;
	for (auto i: m_peers)
		if (auto j = i.lock())
		{
			if (!haveLocal)
				ret.push_back(j->m_socket.local_endpoint());
			haveLocal = true;
			ret.push_back(j->m_socket.remote_endpoint());
		}
	return ret;
}

void PeerServer::doAccept()
{
	cout << "Listening on " << m_acceptor.local_endpoint() << endl;
	m_acceptor.async_accept(m_socket, [&](boost::system::error_code ec)
	{
		if (!ec)
		{
			std::cout << "Accepted connection from " << m_socket.remote_endpoint() << std::endl;
			auto p = std::make_shared<PeerSession>(this, std::move(m_socket), m_requiredNetworkId);
			m_peers.push_back(p);
			p->start();
		}
		doAccept();
	});
}

bool PeerServer::connect(string const& _addr, uint _port)
{
	bi::tcp::resolver resolver(m_ioService);
	cout << "Attempting connection to " << _addr << ":" << dec << _port << endl;
	try
	{
		bi::tcp::socket s(m_ioService);
		boost::asio::connect(s, resolver.resolve({ _addr, toString(_port) }));
		auto p = make_shared<PeerSession>(this, std::move(s), m_requiredNetworkId);
		m_peers.push_back(p);
		cout << "Connected." << endl;
		p->start();
		return true;
	}
	catch (exception& _e)
	{
		cout << "Connection refused (" << _e.what() << ")" << endl;
		return false;
	}
}

bool PeerServer::connect(bi::tcp::endpoint _ep)
{
	bi::tcp::resolver resolver(m_ioService);
	cout << "Attempting connection to " << _ep << endl;
	try
	{
		bi::tcp::socket s(m_ioService);
		boost::asio::connect(s, resolver.resolve(_ep));
		auto p = make_shared<PeerSession>(this, std::move(s), m_requiredNetworkId);
		m_peers.push_back(p);
		cout << "Connected." << endl;
		p->start();
		return true;
	}
	catch (exception& _e)
	{
		cout << "Connection refused (" << _e.what() << ")" << endl;
		return false;
	}
}

void PeerServer::process(BlockChain& _bc)
{
	m_ioService.poll();
	for (auto i = m_peers.begin(); i != m_peers.end();)
		if (auto j = i->lock())
			if (j->m_socket.is_open())
				++i;
			else
				i = m_peers.erase(i);
		else
			i = m_peers.erase(i);
}

void PeerServer::process(BlockChain& _bc, TransactionQueue& _tq, Overlay& _o)
{
	if (m_latestBlockSent == h256())
	{
		// First time - just initialise.
		m_latestBlockSent = _bc.currentHash();
		for (auto const& i: _tq.transactions())
			m_transactionsSent.insert(i.first);
		m_lastPeersRequest = chrono::steady_clock::now();
	}

	process(_bc);

	for (auto it = m_incomingTransactions.begin(); it != m_incomingTransactions.end(); ++it)
		if (!_tq.import(*it))
			m_transactionsSent.insert(sha3(*it));	// if we already had the transaction, then don't bother sending it on.
	m_incomingTransactions.clear();

	// Send any new transactions.
	for (auto j: m_peers)
		if (auto p = j.lock())
		{
			bytes b;
			uint n = 0;
			for (auto const& i: _tq.transactions())
				if (!m_transactionsSent.count(i.first) && !p->m_knownTransactions.count(i.first))
				{
					b += i.second;
					++n;
					m_transactionsSent.insert(i.first);
				}
			if (n)
			{
				RLPStream ts;
				PeerSession::prep(ts);
				ts.appendList(2) << Transactions;
				ts.appendList(n).appendRaw(b).swapOut(b);
				PeerSession::seal(b);
				p->send(&b);
			}
			p->m_knownTransactions.clear();
		}

	// Send any new blocks.
	{
		auto h = _bc.currentHash();
		if (h != m_latestBlockSent)
		{
			// TODO: find where they diverge and send complete new branch.
			RLPStream ts;
			PeerSession::prep(ts);
			ts.appendList(2) << Blocks;
			bytes b;
			ts.appendList(1).appendRaw(_bc.block(_bc.currentHash())).swapOut(b);
			PeerSession::seal(b);
			for (auto j: m_peers)
				if (auto p = j.lock())
				{
					if (!p->m_knownBlocks.count(_bc.currentHash()))
						p->send(&b);
					p->m_knownBlocks.clear();
				}
		}
		m_latestBlockSent = h;
	}

	for (bool accepted = 1; accepted;)
	{
		accepted = 0;
		if (m_incomingBlocks.size())
			for (auto it = prev(m_incomingBlocks.end());; --it)
			{
				try
				{
					_bc.import(*it, _o);
					it = m_incomingBlocks.erase(it);
					++accepted;
				}
				catch (UnknownParent)
				{
					// Don't (yet) know its parent. Leave it for later.
				}
				catch (...)
				{
					// Some other error - erase it.
					it = m_incomingBlocks.erase(it);
				}

				if (it == m_incomingBlocks.begin())
					break;
			}
	}

	// Connect to additional peers
	// TODO: Need to avoid connecting to self & existing peers. Existing peers is easy, but need portable method of listing all addresses we can listen to avoid self.
	/*
	while (m_peers.size() < m_idealPeerCount)
	{
		if (m_incomingPeers.empty())
		{
			if (chrono::steady_clock::now() - m_lastPeersRequest > chrono::seconds(10))
			{
				RLPStream s;
				bytes b;
				(PeerSession::prep(s).appendList(1) << GetPeers).swapOut(b);
				PeerSession::seal(b);
				for (auto const& i: m_peers)
					if (auto p = i.lock())
						p->send(&b);
				m_lastPeersRequest = chrono::steady_clock::now();
			}
			break;
		}
		connect(m_incomingPeers.back());
		m_incomingPeers.pop_back();
	}*/
}

std::vector<PeerInfo> PeerServer::peers() const
{
	const_cast<PeerServer*>(this)->pingAll();
	usleep(200000);
	std::vector<PeerInfo> ret;
	for (auto& i: m_peers)
		if (auto j = i.lock())
			if (j->m_socket.is_open())
				ret.push_back(j->m_info);
	return ret;
}

void PeerServer::pingAll()
{
	for (auto& i: m_peers)
		if (auto j = i.lock())
			j->ping();
}
