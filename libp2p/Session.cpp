// Aleth: Ethereum C++ client, tools and libraries.
// Copyright 2019 Aleth Authors.
// Licensed under the GNU General Public License, Version 3.

#include "Session.h"

#include "Host.h"
#include "RLPXFrameCoder.h"
#include <libdevcore/Common.h>
#include <libdevcore/CommonIO.h>
#include <libdevcore/Exceptions.h>
#include <chrono>

using namespace std;
using namespace dev;
using namespace dev::p2p;

Session::Session(Host* _h, unique_ptr<RLPXFrameCoder>&& _io, std::shared_ptr<RLPXSocket> const& _s,
    std::shared_ptr<Peer> const& _n, PeerSessionInfo _info)
  : m_server(_h),
    m_io(move(_io)),
    m_socket(_s),
    m_peer(_n),
    m_info(_info),
    m_ping(chrono::steady_clock::time_point::max()),
    m_logContext(_info.id.abridged() + "|" + _info.clientVersion)
{
    m_peer->m_lastDisconnect = NoDisconnect;
    m_lastReceived = m_connect = chrono::steady_clock::now();
}

Session::~Session()
{
    LOG_SCOPED_CONTEXT(m_logContext);

    cnetlog << "Closing peer session :-(";
    m_peer->m_lastConnected = m_peer->m_lastAttempted - chrono::seconds(1);

    // Read-chain finished for one reason or another.
    for (auto& i : m_capabilities)
    {
        i.second->onDisconnect(id());
        i.second.reset();
    }

    try
    {
        bi::tcp::socket& socket = m_socket->ref();
        if (socket.is_open())
        {
            boost::system::error_code ec;
            socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
            socket.close();
        }
    }
    catch (...){}
}

ReputationManager& Session::repMan()
{
    return m_server->repMan();
}

NodeID Session::id() const
{
    return m_peer ? m_peer->id : NodeID();
}

void Session::addRating(int _r)
{
    if (m_peer)
    {
        m_peer->m_rating += _r;
        m_peer->m_score += _r;
        if (_r >= 0)
            m_peer->noteSessionGood();
    }
}

int Session::rating() const
{
    return m_peer->m_rating;
}

bool Session::readPacket(uint16_t _capId, unsigned _packetType, RLP const& _r)
{
    m_lastReceived = chrono::steady_clock::now();
    clog(VerbosityTrace, "net") << "Received " << capabilityPacketTypeToString(_packetType) << "("
                                << _packetType << ") from " << m_info.id << "@"
                                << m_socket->remoteEndpoint();
    try // Generic try-catch block designed to capture RLP format errors - TODO: give decent diagnostics, make a bit more specific over what is caught.
    {
        // v4 frame headers are useless, offset packet type used
        // v5 protocol type is in header, packet type not offset
        if (_capId == 0 && _packetType < UserPacket)
            return interpret(static_cast<P2pPacketType>(_packetType), _r);

        for (auto const& cap : m_capabilities)
        {
            auto const& name = cap.first.first;
            auto const& capability = cap.second;

            if (canHandle(name, capability->messageCount(), _packetType))
            {
                if (!capabilityEnabled(name))
                    return true;

                auto offset = capabilityOffset(name);
                assert(offset);
                return capability->interpretCapabilityPacket(id(), _packetType - *offset, _r);
            }
        }

        return false;
    }
    catch (std::exception const& _e)
    {
        cnetlog << "Exception caught in p2p::Session::interpret(): " << _e.what()
                << ". PacketType: " << _packetType << ". RLP: " << _r;
        disconnect(BadProtocol);
        return true;
    }
    return true;
}

bool Session::interpret(P2pPacketType _t, RLP const& _r)
{
    clog(VerbosityTrace, "p2pcap")
        << p2pPacketTypeToString(_t) << " from " << m_info.id << "@" << m_socket->remoteEndpoint();
    switch (_t)
    {
    case DisconnectPacket:
    {
        string reason = "Unspecified";
        auto r = (DisconnectReason)_r[0].toInt<int>();
        if (!_r[0].isInt())
            drop(BadProtocol);
        else
        {
            reason = reasonOf(r);
            clog(VerbosityDebug, "p2pcap") << "Disconnect (reason: " << reason << ") from " << m_info.id << "@"
                    << m_socket->remoteEndpoint();
            drop(DisconnectRequested);
        }
        break;
    }
    case PingPacket:
    {
        clog(VerbosityTrace, "p2pcap") << "Pong to " << m_info.id << "@" << m_socket->remoteEndpoint();
        RLPStream s;
        sealAndSend(prep(s, PongPacket));
        break;
    }
    case PongPacket:
        DEV_GUARDED(x_info)
        {
            m_info.lastPing = std::chrono::steady_clock::now() - m_ping;
            clog(VerbosityTrace, "p2pcap") << "Ping latency: "
                        << chrono::duration_cast<chrono::milliseconds>(m_info.lastPing).count()
                        << " ms";
        }
        break;
    default:
        return false;
    }
    return true;
}

void Session::ping()
{
    clog(VerbosityTrace, "p2pcap") << "Ping to " << m_info.id << "@" << m_socket->remoteEndpoint();
    RLPStream s;
    sealAndSend(prep(s, PingPacket));
    m_ping = std::chrono::steady_clock::now();
}

RLPStream& Session::prep(RLPStream& _s, P2pPacketType _id, unsigned _args)
{
    return _s.append((unsigned)_id).appendList(_args);
}

void Session::sealAndSend(RLPStream& _s)
{
    bytes b;
    _s.swapOut(b);
    send(move(b));
}

bool Session::checkPacket(bytesConstRef _msg)
{
    if (_msg[0] > 0x7f || _msg.size() < 2)
        return false;
    if (RLP(_msg.cropped(1)).actualSize() + 1 != _msg.size())
        return false;
    return true;
}

void Session::send(bytes&& _msg)
{
    bytesConstRef msg(&_msg);
    clog(VerbosityTrace, "net") << "Sending " << capabilityPacketTypeToString(_msg[0]) << " to "
                                << m_info.id << "@" << m_socket->remoteEndpoint();
    if (!checkPacket(msg))
        cnetlog << "INVALID PACKET CONSTRUCTED!";

    if (!m_socket->ref().is_open())
        return;

    bool doWrite = false;
    DEV_GUARDED(x_framing)
    {
        m_writeQueue.push_back(std::move(_msg));
        doWrite = (m_writeQueue.size() == 1);
    }

    if (doWrite)
        write();
}

void Session::write()
{
    bytes const* out = nullptr;
    DEV_GUARDED(x_framing)
    {
        m_io->writeSingleFramePacket(&m_writeQueue[0], m_writeQueue[0]);
        out = &m_writeQueue[0];
    }
    auto self(shared_from_this());
    ba::async_write(m_socket->ref(), ba::buffer(*out),
        [this, self](boost::system::error_code ec, std::size_t /*length*/) {
            LOG_SCOPED_CONTEXT(m_logContext);

            // must check queue, as write callback can occur following dropped()
            if (ec)
            {
                cnetlog << "Error sending: " << ec.message();
                drop(TCPError);
                return;
            }

            DEV_GUARDED(x_framing)
            {
                m_writeQueue.pop_front();
                if (m_writeQueue.empty())
                    return;
            }
            write();
        });
}

namespace
{
    void halveAtomicInt(atomic<int>& i)
    {
        // atomic<int> doesn't have /= operator, so we do it manually
        int oldInt = 0;
        int newInt = 0;
        do
        {
            oldInt = i;
            newInt = oldInt / 2;
            // Current value could already change when we get to exchange,
            // we'll need to retry in the loop in this case
        } while (!i.atomic::compare_exchange_weak(oldInt, newInt));
    }
}

void Session::drop(DisconnectReason _reason)
{
    if (m_dropped)
        return;
    bi::tcp::socket& socket = m_socket->ref();
    if (socket.is_open())
        try
        {
            boost::system::error_code ec;
            cnetdetails << "Closing " << socket.remote_endpoint(ec) << " (" << reasonOf(_reason)
                        << ")";
            socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
            socket.close();
        }
        catch (...) {}

    m_peer->m_lastDisconnect = _reason;
    if (_reason == BadProtocol)
    {
        halveAtomicInt(m_peer->m_rating);
        halveAtomicInt(m_peer->m_score);
    }
    m_dropped = true;
}

void Session::disconnect(DisconnectReason _reason)
{
    cnetdetails << "Disconnecting (our reason: " << reasonOf(_reason) << ") from " << m_info.id
                << "@" << m_socket->remoteEndpoint();

    if (m_socket->ref().is_open())
    {
        RLPStream s;
        prep(s, DisconnectPacket, 1) << (int)_reason;
        sealAndSend(s);
    }
    drop(_reason);
}

void Session::start()
{
    ping();
    doRead();
}

void Session::doRead()
{
    // ignore packets received while waiting to disconnect.
    if (m_dropped)
        return;

    auto self(shared_from_this());
    m_data.resize(h256::size);
    ba::async_read(m_socket->ref(), boost::asio::buffer(m_data, h256::size),
        [this, self](boost::system::error_code ec, std::size_t length) {
            LOG_SCOPED_CONTEXT(m_logContext);

            if (!checkRead(h256::size, ec, length))
                return;
            else if (!m_io->authAndDecryptHeader(bytesRef(m_data.data(), length)))
            {
                cnetlog << "header decrypt failed";
                drop(BadProtocol);  // todo: better error
                return;
            }

            uint16_t hProtocolId;
            uint32_t hLength;
            uint8_t hPadding;
            try
            {
                RLPXFrameInfo header(bytesConstRef(m_data.data(), length));
                hProtocolId = header.protocolId;
                hLength = header.length;
                hPadding = header.padding;
            }
            catch (std::exception const& _e)
            {
                cnetlog << "Exception decoding frame header RLP: " << _e.what() << " "
                        << bytesConstRef(m_data.data(), h128::size).cropped(3);
                drop(BadProtocol);
                return;
            }

            /// read padded frame and mac
            auto tlen = hLength + hPadding + h128::size;
            m_data.resize(tlen);
            ba::async_read(m_socket->ref(), boost::asio::buffer(m_data, tlen),
                [this, self, hLength, hProtocolId, tlen](
                    boost::system::error_code ec, std::size_t length) {
                    LOG_SCOPED_CONTEXT(m_logContext);

                    if (!checkRead(tlen, ec, length))
                        return;
                    else if (!m_io->authAndDecryptFrame(bytesRef(m_data.data(), tlen)))
                    {
                        cnetlog << "frame decrypt failed";
                        drop(BadProtocol);  // todo: better error
                        return;
                    }

                    bytesConstRef frame(m_data.data(), hLength);
                    if (!checkPacket(frame))
                    {
                        cerr << "Received " << frame.size() << ": " << toHex(frame) << endl;
                        cnetlog << "INVALID MESSAGE RECEIVED";
                        disconnect(BadProtocol);
                        return;
                    }
                    else
                    {
                        auto packetType = static_cast<P2pPacketType>(RLP(frame.cropped(0, 1)).toInt<unsigned>());
                        RLP r(frame.cropped(1));
                        bool ok = readPacket(hProtocolId, packetType, r);
                        if (!ok)
                            cnetlog << "Couldn't interpret packet. " << RLP(r);
                    }
                    doRead();
                });
        });
}

bool Session::checkRead(std::size_t _expected, boost::system::error_code _ec, std::size_t _length)
{
    if (_ec && _ec.category() != boost::asio::error::get_misc_category() && _ec.value() != boost::asio::error::eof)
    {
        cnetdetails << "Error reading: " << _ec.message();
        drop(TCPError);
        return false;
    }
    else if (_ec && _length < _expected)
    {
        cnetlog << "Error reading - Abrupt peer disconnect: " << _ec.message();
        repMan().noteRude(*this);
        drop(TCPError);
        return false;
    }
    else if (_length != _expected)
    {
        // with static m_data-sized buffer this shouldn't happen unless there's a regression
        // sec recommends checking anyways (instead of assert)
        cnetlog << "Error reading - TCP read buffer length differs from expected frame size.";
        disconnect(UserReason);
        return false;
    }

    return true;
}

void Session::registerCapability(
    CapDesc const& _desc, unsigned _offset, std::shared_ptr<CapabilityFace> _p)
{
    DEV_GUARDED(x_framing)
    {
        m_capabilities[_desc] = move(_p);
        m_capabilityOffsets[_desc.first] = _offset;
    }
}

bool Session::canHandle(
    std::string const& _capability, unsigned _messageCount, unsigned _packetType) const
{
    auto const offset = capabilityOffset(_capability);

    return offset && _packetType >= *offset && _packetType < _messageCount + *offset;
}

void Session::disableCapability(std::string const& _capabilityName, std::string const& _problem)
{
    cnetdetails << "DISABLE: Disabling capability '" << _capabilityName
                << "'. Reason: " << _problem;
    m_disabledCapabilities.insert(_capabilityName);
}

boost::optional<unsigned> Session::capabilityOffset(std::string const& _capabilityName) const
{
    auto it = m_capabilityOffsets.find(_capabilityName);
    return it == m_capabilityOffsets.end() ? boost::optional<unsigned>{} : it->second;
}

char const* Session::capabilityPacketTypeToString(unsigned _packetType) const
{
    if (_packetType < UserPacket)
        return p2pPacketTypeToString(static_cast<P2pPacketType>(_packetType));
    for (auto capIter : m_capabilities)
    {
        auto const& capName = capIter.first.first;
        auto cap = capIter.second;
        if (canHandle(capName, cap->messageCount(), _packetType))
        {
            auto offset = capabilityOffset(capName);
            assert(offset);
            return cap->packetTypeToString(_packetType - *offset);
        }
    }
    return "Unknown";
}
