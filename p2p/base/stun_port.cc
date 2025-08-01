/*
 *  Copyright 2004 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "p2p/base/stun_port.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "absl/memory/memory.h"
#include "absl/strings/string_view.h"
#include "api/async_dns_resolver.h"
#include "api/candidate.h"
#include "api/field_trials_view.h"
#include "api/packet_socket_factory.h"
#include "api/transport/stun.h"
#include "p2p/base/connection.h"
#include "p2p/base/p2p_constants.h"
#include "p2p/base/port.h"
#include "p2p/base/port_interface.h"
#include "p2p/base/stun_request.h"
#include "rtc_base/async_packet_socket.h"
#include "rtc_base/checks.h"
#include "rtc_base/dscp.h"
#include "rtc_base/ip_address.h"
#include "rtc_base/logging.h"
#include "rtc_base/net_helper.h"
#include "rtc_base/network.h"
#include "rtc_base/network/received_packet.h"
#include "rtc_base/network/sent_packet.h"
#include "rtc_base/socket.h"
#include "rtc_base/socket_address.h"
#include "rtc_base/strings/string_builder.h"
#include "rtc_base/time_utils.h"
#include "system_wrappers/include/metrics.h"

namespace webrtc {

// TODO(?): Move these to a common place (used in relayport too)
const int RETRY_TIMEOUT = 50 * 1000;  // 50 seconds

// Stop logging errors in UDPPort::SendTo after we have logged
// `kSendErrorLogLimit` messages. Start again after a successful send.
const int kSendErrorLogLimit = 5;

// Handles a binding request sent to the STUN server.
class StunBindingRequest : public StunRequest {
 public:
  StunBindingRequest(UDPPort* port,
                     const SocketAddress& addr,
                     int64_t start_time)
      : StunRequest(port->request_manager(),
                    std::make_unique<StunMessage>(STUN_BINDING_REQUEST)),
        port_(port),
        server_addr_(addr),
        start_time_(start_time) {
    SetAuthenticationRequired(false);
  }

  const SocketAddress& server_addr() const { return server_addr_; }

  void OnResponse(StunMessage* response) override {
    const StunAddressAttribute* addr_attr =
        response->GetAddress(STUN_ATTR_MAPPED_ADDRESS);
    if (!addr_attr) {
      RTC_LOG(LS_ERROR) << "Binding response missing mapped address.";
    } else if (addr_attr->family() != STUN_ADDRESS_IPV4 &&
               addr_attr->family() != STUN_ADDRESS_IPV6) {
      RTC_LOG(LS_ERROR) << "Binding address has bad family";
    } else {
      SocketAddress addr(addr_attr->ipaddr(), addr_attr->port());
      port_->OnStunBindingRequestSucceeded(this->Elapsed(), server_addr_, addr);
    }

    // The keep-alive requests will be stopped after its lifetime has passed.
    if (WithinLifetime(TimeMillis())) {
      port_->request_manager_.SendDelayed(
          new StunBindingRequest(port_, server_addr_, start_time_),
          port_->stun_keepalive_delay());
    }
  }

  void OnErrorResponse(StunMessage* response) override {
    const StunErrorCodeAttribute* attr = response->GetErrorCode();
    if (!attr) {
      RTC_LOG(LS_ERROR) << "Missing binding response error code.";
    } else {
      RTC_LOG(LS_ERROR) << "Binding error response:"
                           " class="
                        << attr->eclass() << " number=" << attr->number()
                        << " reason=" << attr->reason();
    }

    port_->OnStunBindingOrResolveRequestFailed(
        server_addr_, attr ? attr->number() : STUN_ERROR_GLOBAL_FAILURE,
        attr ? attr->reason()
             : "STUN binding response with no error code attribute.");

    int64_t now = TimeMillis();
    if (WithinLifetime(now) && TimeDiff(now, start_time_) < RETRY_TIMEOUT) {
      port_->request_manager_.SendDelayed(
          new StunBindingRequest(port_, server_addr_, start_time_),
          port_->stun_keepalive_delay());
    }
  }
  void OnTimeout() override {
    RTC_LOG(LS_ERROR) << "Binding request timed out from "
                      << port_->GetLocalAddress().ToSensitiveString() << " ("
                      << port_->Network()->name() << ")";
    port_->OnStunBindingOrResolveRequestFailed(
        server_addr_, STUN_ERROR_SERVER_NOT_REACHABLE,
        "STUN binding request timed out.");
  }

 private:
  // Returns true if `now` is within the lifetime of the request (a negative
  // lifetime means infinite).
  bool WithinLifetime(int64_t now) const {
    int lifetime = port_->stun_keepalive_lifetime();
    return lifetime < 0 || TimeDiff(now, start_time_) <= lifetime;
  }

  UDPPort* port_;
  const SocketAddress server_addr_;

  int64_t start_time_;
};

UDPPort::AddressResolver::AddressResolver(
    PacketSocketFactory* factory,
    std::function<void(const SocketAddress&, int)> done_callback)
    : socket_factory_(factory), done_(std::move(done_callback)) {}

void UDPPort::AddressResolver::Resolve(
    const SocketAddress& address,
    int family,
    const FieldTrialsView& /* field_trials */) {
  if (resolvers_.find(address) != resolvers_.end())
    return;

  auto resolver = socket_factory_->CreateAsyncDnsResolver();
  auto resolver_ptr = resolver.get();
  std::pair<SocketAddress, std::unique_ptr<AsyncDnsResolverInterface>> pair =
      std::make_pair(address, std::move(resolver));

  resolvers_.insert(std::move(pair));
  auto callback = [this, address] {
    ResolverMap::const_iterator it = resolvers_.find(address);
    if (it != resolvers_.end()) {
      done_(it->first, it->second->result().GetError());
    }
  };
  resolver_ptr->Start(address, family, std::move(callback));
}

bool UDPPort::AddressResolver::GetResolvedAddress(const SocketAddress& input,
                                                  int family,
                                                  SocketAddress* output) const {
  ResolverMap::const_iterator it = resolvers_.find(input);
  if (it == resolvers_.end())
    return false;

  return it->second->result().GetResolvedAddress(family, output);
}

UDPPort::UDPPort(const PortParametersRef& args,
                 IceCandidateType type,
                 AsyncPacketSocket* socket,
                 bool emit_local_for_anyaddress)
    : Port(args, type),
      request_manager_(
          args.network_thread,
          [this](const void* data, size_t size, StunRequest* request) {
            OnSendPacket(data, size, request);
          }),
      socket_(socket),
      error_(0),
      ready_(false),
      stun_keepalive_delay_(STUN_KEEPALIVE_INTERVAL),
      dscp_(DSCP_NO_CHANGE),
      emit_local_for_anyaddress_(emit_local_for_anyaddress) {}

UDPPort::UDPPort(const PortParametersRef& args,
                 IceCandidateType type,
                 uint16_t min_port,
                 uint16_t max_port,
                 bool emit_local_for_anyaddress)
    : Port(args, type, min_port, max_port),
      request_manager_(
          args.network_thread,
          [this](const void* data, size_t size, StunRequest* request) {
            OnSendPacket(data, size, request);
          }),
      socket_(nullptr),
      error_(0),
      ready_(false),
      stun_keepalive_delay_(STUN_KEEPALIVE_INTERVAL),
      dscp_(DSCP_NO_CHANGE),
      emit_local_for_anyaddress_(emit_local_for_anyaddress) {}

bool UDPPort::Init() {
  stun_keepalive_lifetime_ = GetStunKeepaliveLifetime();
  if (!SharedSocket()) {
    RTC_DCHECK(socket_ == nullptr);
    socket_ = socket_factory()->CreateUdpSocket(
        SocketAddress(Network()->GetBestIP(), 0), min_port(), max_port());
    if (!socket_) {
      RTC_LOG(LS_WARNING) << ToString() << ": UDP socket creation failed";
      return false;
    }
    socket_->RegisterReceivedPacketCallback(
        [&](AsyncPacketSocket* socket, const ReceivedIpPacket& packet) {
          OnReadPacket(socket, packet);
        });
  }
  socket_->SignalSentPacket.connect(this, &UDPPort::OnSentPacket);
  socket_->SignalReadyToSend.connect(this, &UDPPort::OnReadyToSend);
  socket_->SignalAddressReady.connect(this, &UDPPort::OnLocalAddressReady);
  return true;
}

UDPPort::~UDPPort() {
  if (!SharedSocket())
    delete socket_;
}

void UDPPort::PrepareAddress() {
  RTC_DCHECK(request_manager_.empty());
  if (socket_->GetState() == AsyncPacketSocket::STATE_BOUND) {
    OnLocalAddressReady(socket_, socket_->GetLocalAddress());
  }
}

void UDPPort::MaybePrepareStunCandidate() {
  // Sending binding request to the STUN server if address is available to
  // prepare STUN candidate.
  if (!server_addresses_.empty()) {
    SendStunBindingRequests();
  } else {
    // Port is done allocating candidates.
    MaybeSetPortCompleteOrError();
  }
}

Connection* UDPPort::CreateConnection(const Candidate& address,
                                      CandidateOrigin /* origin */) {
  if (!SupportsProtocol(address.protocol())) {
    return nullptr;
  }

  if (!IsCompatibleAddress(address.address())) {
    return nullptr;
  }

  // In addition to DCHECK-ing the non-emptiness of local candidates, we also
  // skip this Port with null if there are latent bugs to violate it; otherwise
  // it would lead to a crash when accessing the local candidate of the
  // connection that would be created below.
  if (Candidates().empty()) {
    RTC_DCHECK_NOTREACHED();
    return nullptr;
  }
  // When the socket is shared, the srflx candidate is gathered by the UDPPort.
  // The assumption here is that
  //  1) if the IP concealment with mDNS is not enabled, the gathering of the
  //     host candidate of this port (which is synchronous),
  //  2) or otherwise if enabled, the start of name registration of the host
  //     candidate (as the start of asynchronous gathering)
  // is always before the gathering of a srflx candidate (and any prflx
  // candidate).
  //
  // See also the definition of MdnsNameRegistrationStatus::kNotStarted in
  // port.h.
  RTC_DCHECK(!SharedSocket() || Candidates()[0].is_local() ||
             mdns_name_registration_status() !=
                 MdnsNameRegistrationStatus::kNotStarted);

  Connection* conn = new ProxyConnection(NewWeakPtr(), 0, address);
  AddOrReplaceConnection(conn);
  return conn;
}

int UDPPort::SendTo(const void* data,
                    size_t size,
                    const SocketAddress& addr,
                    const AsyncSocketPacketOptions& options,
                    bool /* payload */) {
  AsyncSocketPacketOptions modified_options(options);
  CopyPortInformationToPacketInfo(&modified_options.info_signaled_after_sent);
  int sent = socket_->SendTo(data, size, addr, modified_options);
  if (sent < 0) {
    error_ = socket_->GetError();
    // Rate limiting added for crbug.com/856088.
    // TODO(webrtc:9622): Use general rate limiting mechanism once it exists.
    if (send_error_count_ < kSendErrorLogLimit) {
      ++send_error_count_;
      RTC_LOG(LS_ERROR) << ToString() << ": UDP send of " << size
                        << " bytes to host "
                        << addr.ToSensitiveNameAndAddressString()
                        << " failed with error " << error_;
    }
  } else {
    send_error_count_ = 0;
  }
  return sent;
}

void UDPPort::UpdateNetworkCost() {
  Port::UpdateNetworkCost();
  stun_keepalive_lifetime_ = GetStunKeepaliveLifetime();
}

DiffServCodePoint UDPPort::StunDscpValue() const {
  return dscp_;
}

int UDPPort::SetOption(Socket::Option opt, int value) {
  if (opt == Socket::OPT_DSCP) {
    // Save value for future packets we instantiate.
    dscp_ = static_cast<DiffServCodePoint>(value);
  }
  return socket_->SetOption(opt, value);
}

int UDPPort::GetOption(Socket::Option opt, int* value) {
  return socket_->GetOption(opt, value);
}

int UDPPort::GetError() {
  return error_;
}

bool UDPPort::HandleIncomingPacket(AsyncPacketSocket* socket,
                                   const ReceivedIpPacket& packet) {
  // All packets given to UDP port will be consumed.
  OnReadPacket(socket, packet);
  return true;
}

bool UDPPort::SupportsProtocol(absl::string_view protocol) const {
  return protocol == UDP_PROTOCOL_NAME;
}

ProtocolType UDPPort::GetProtocol() const {
  return PROTO_UDP;
}

void UDPPort::GetStunStats(std::optional<StunStats>* stats) {
  *stats = stats_;
}

void UDPPort::set_stun_keepalive_delay(const std::optional<int>& delay) {
  stun_keepalive_delay_ = delay.value_or(STUN_KEEPALIVE_INTERVAL);
}

void UDPPort::OnLocalAddressReady(AsyncPacketSocket* /* socket */,
                                  const SocketAddress& address) {
  // When adapter enumeration is disabled and binding to the any address, the
  // default local address will be issued as a candidate instead if
  // `emit_local_for_anyaddress` is true. This is to allow connectivity for
  // applications which absolutely requires a HOST candidate.
  SocketAddress addr = address;

  // If MaybeSetDefaultLocalAddress fails, we keep the "any" IP so that at
  // least the port is listening.
  MaybeSetDefaultLocalAddress(&addr);

  AddAddress(addr, addr, SocketAddress(), UDP_PROTOCOL_NAME, "", "",
             IceCandidateType::kHost, ICE_TYPE_PREFERENCE_HOST, 0, "", false);
  MaybePrepareStunCandidate();
}

void UDPPort::PostAddAddress(bool /* is_final */) {
  MaybeSetPortCompleteOrError();
}

void UDPPort::OnReadPacket(AsyncPacketSocket* socket,
                           const ReceivedIpPacket& packet) {
  RTC_DCHECK(socket == socket_);
  RTC_DCHECK(!packet.source_address().IsUnresolvedIP());

  // Look for a response from the STUN server.
  // Even if the response doesn't match one of our outstanding requests, we
  // will eat it because it might be a response to a retransmitted packet, and
  // we already cleared the request when we got the first response.
  if (server_addresses_.find(packet.source_address()) !=
      server_addresses_.end()) {
    request_manager_.CheckResponse(
        reinterpret_cast<const char*>(packet.payload().data()),
        packet.payload().size());
    return;
  }

  if (Connection* conn = GetConnection(packet.source_address())) {
    conn->OnReadPacket(packet);
  } else {
    Port::OnReadPacket(packet, PROTO_UDP);
  }
}

void UDPPort::OnSentPacket(AsyncPacketSocket* /* socket */,
                           const SentPacketInfo& sent_packet) {
  PortInterface::SignalSentPacket(sent_packet);
}

void UDPPort::OnReadyToSend(AsyncPacketSocket* /* socket */) {
  Port::OnReadyToSend();
}

void UDPPort::SendStunBindingRequests() {
  // We will keep pinging the stun server to make sure our NAT pin-hole stays
  // open until the deadline (specified in SendStunBindingRequest).
  RTC_DCHECK(request_manager_.empty());

  for (ServerAddresses::const_iterator it = server_addresses_.begin();
       it != server_addresses_.end();) {
    // sending a STUN binding request may cause the current SocketAddress to be
    // erased from the set, invalidating the loop iterator before it is
    // incremented (even if the SocketAddress itself still exists). So make a
    // copy of the loop iterator, which may be safely invalidated.
    ServerAddresses::const_iterator addr = it++;
    SendStunBindingRequest(*addr);
  }
}

void UDPPort::ResolveStunAddress(const SocketAddress& stun_addr) {
  if (!resolver_) {
    resolver_.reset(new AddressResolver(
        socket_factory(), [&](const SocketAddress& input, int error) {
          OnResolveResult(input, error);
        }));
  }

  RTC_LOG(LS_INFO) << ToString() << ": Starting STUN host lookup for "
                   << stun_addr.ToSensitiveString();
  resolver_->Resolve(stun_addr, Network()->family(), field_trials());
}

void UDPPort::OnResolveResult(const SocketAddress& input, int error) {
  RTC_DCHECK(resolver_.get() != nullptr);

  SocketAddress resolved;
  if (error != 0 || !resolver_->GetResolvedAddress(
                        input, Network()->GetBestIP().family(), &resolved)) {
    RTC_LOG(LS_WARNING) << ToString()
                        << ": StunPort: stun host lookup received error "
                        << error;
    OnStunBindingOrResolveRequestFailed(input, STUN_ERROR_SERVER_NOT_REACHABLE,
                                        "STUN host lookup received error.");
    return;
  }

  server_addresses_.erase(input);

  if (server_addresses_.find(resolved) == server_addresses_.end()) {
    server_addresses_.insert(resolved);
    SendStunBindingRequest(resolved);
  }
}

void UDPPort::SendStunBindingRequest(const SocketAddress& stun_addr) {
  if (stun_addr.IsUnresolvedIP()) {
    ResolveStunAddress(stun_addr);
    return;
  }

  if (socket_->GetState() != AsyncPacketSocket::STATE_BOUND) {
    return;
  }

    // Check if `server_addr_` is compatible with the port's ip.
  if (!IsCompatibleAddress(stun_addr)) {
    // Since we can't send stun messages to the server, we should mark this
    // port ready. This is not an error but similar to ignoring
    // a mismatch of the address family when pairing candidates.
    RTC_LOG(LS_WARNING) << ToString()
                        << ": STUN server address is incompatible.";
    OnStunBindingOrResolveRequestFailed(stun_addr, STUN_ERROR_NOT_AN_ERROR,
                                        "STUN server address is incompatible.");
    return;
  }

  RTC_HISTOGRAM_ENUMERATION("WebRTC.PeerConnection.Stun.ServerAddressType",
                            static_cast<int>(stun_addr.GetIPAddressType()),
                            static_cast<int>(IPAddressType::kMaxValue));

  request_manager_.Send(new StunBindingRequest(this, stun_addr, TimeMillis()));
}

bool UDPPort::MaybeSetDefaultLocalAddress(SocketAddress* addr) const {
  if (!addr->IsAnyIP() || !emit_local_for_anyaddress_ ||
      !Network()->default_local_address_provider()) {
    return true;
  }
  IPAddress default_address;
  bool result =
      Network()->default_local_address_provider()->GetDefaultLocalAddress(
          addr->family(), &default_address);
  if (!result || default_address.IsNil()) {
    return false;
  }

  addr->SetIP(default_address);
  return true;
}

void UDPPort::OnStunBindingRequestSucceeded(
    int rtt_ms,
    const SocketAddress& stun_server_addr,
    const SocketAddress& stun_reflected_addr) {
  RTC_DCHECK(stats_.stun_binding_responses_received <
             stats_.stun_binding_requests_sent);
  stats_.stun_binding_responses_received++;
  stats_.stun_binding_rtt_ms_total += rtt_ms;
  stats_.stun_binding_rtt_ms_squared_total += rtt_ms * rtt_ms;
  if (bind_request_succeeded_servers_.find(stun_server_addr) !=
      bind_request_succeeded_servers_.end()) {
    return;
  }
  bind_request_succeeded_servers_.insert(stun_server_addr);
  // If socket is shared and `stun_reflected_addr` is equal to local socket
  // address and mDNS obfuscation is not enabled, or if the same address has
  // been added by another STUN server, then discarding the stun address.
  // For STUN, related address is the local socket address.
  if ((!SharedSocket() || stun_reflected_addr != socket_->GetLocalAddress() ||
       Network()->GetMdnsResponder() != nullptr) &&
      !HasStunCandidateWithAddress(stun_reflected_addr)) {
    SocketAddress related_address = socket_->GetLocalAddress();
    // If we can't stamp the related address correctly, empty it to avoid leak.
    if (!MaybeSetDefaultLocalAddress(&related_address)) {
      related_address = EmptySocketAddressWithFamily(related_address.family());
    }

    StringBuilder url;
    url << "stun:" << stun_server_addr.hostname() << ":"
        << stun_server_addr.port();
    AddAddress(stun_reflected_addr, socket_->GetLocalAddress(), related_address,
               UDP_PROTOCOL_NAME, "", "", IceCandidateType::kSrflx,
               ICE_TYPE_PREFERENCE_SRFLX, 0, url.str(), false);
  }
  MaybeSetPortCompleteOrError();
}

void UDPPort::OnStunBindingOrResolveRequestFailed(
    const SocketAddress& stun_server_addr,
    int error_code,
    absl::string_view reason) {
  if (error_code != STUN_ERROR_NOT_AN_ERROR) {
    StringBuilder url;
    url << "stun:" << stun_server_addr.ToString();
    SignalCandidateError(
        this, IceCandidateErrorEvent(
                  GetLocalAddress().HostAsSensitiveURIString(),
                  GetLocalAddress().port(), url.str(), error_code, reason));
  }
  if (bind_request_failed_servers_.find(stun_server_addr) !=
      bind_request_failed_servers_.end()) {
    return;
  }
  bind_request_failed_servers_.insert(stun_server_addr);
  MaybeSetPortCompleteOrError();
}

void UDPPort::MaybeSetPortCompleteOrError() {
  if (mdns_name_registration_status() ==
      MdnsNameRegistrationStatus::kInProgress) {
    return;
  }

  if (ready_) {
    return;
  }

  // Do not set port ready if we are still waiting for bind responses.
  const size_t servers_done_bind_request =
      bind_request_failed_servers_.size() +
      bind_request_succeeded_servers_.size();
  if (server_addresses_.size() != servers_done_bind_request) {
    return;
  }

  // Setting ready status.
  ready_ = true;

  // The port is "completed" if there is no stun server provided, or the bind
  // request succeeded for any stun server, or the socket is shared.
  if (server_addresses_.empty() || bind_request_succeeded_servers_.size() > 0 ||
      SharedSocket()) {
    SignalPortComplete(this);
  } else {
    SignalPortError(this);
  }
}

// TODO(?): merge this with SendTo above.
void UDPPort::OnSendPacket(const void* data, size_t size, StunRequest* req) {
  StunBindingRequest* sreq = static_cast<StunBindingRequest*>(req);
  AsyncSocketPacketOptions options(StunDscpValue());
  options.info_signaled_after_sent.packet_type = PacketType::kStunMessage;
  CopyPortInformationToPacketInfo(&options.info_signaled_after_sent);
  if (socket_->SendTo(data, size, sreq->server_addr(), options) < 0) {
    RTC_LOG_ERR_EX(LS_ERROR, socket_->GetError())
        << "UDP send of " << size << " bytes to host "
        << sreq->server_addr().ToSensitiveNameAndAddressString()
        << " failed with error " << error_;
  }
  stats_.stun_binding_requests_sent++;
}

bool UDPPort::HasStunCandidateWithAddress(const SocketAddress& addr) const {
  const std::vector<Candidate>& existing_candidates = Candidates();
  std::vector<Candidate>::const_iterator it = existing_candidates.begin();
  for (; it != existing_candidates.end(); ++it) {
    if (it->is_stun() && it->address() == addr)
      return true;
  }
  return false;
}

std::unique_ptr<StunPort> StunPort::Create(
    const PortParametersRef& args,
    uint16_t min_port,
    uint16_t max_port,
    const ServerAddresses& servers,
    std::optional<int> stun_keepalive_interval) {
  // Using `new` to access a non-public constructor.
  auto port = absl::WrapUnique(new StunPort(args, min_port, max_port, servers));
  port->set_stun_keepalive_delay(stun_keepalive_interval);
  if (!port->Init()) {
    return nullptr;
  }
  return port;
}

StunPort::StunPort(const PortParametersRef& args,
                   uint16_t min_port,
                   uint16_t max_port,
                   const ServerAddresses& servers)
    : UDPPort(args, IceCandidateType::kSrflx, min_port, max_port, false) {
  set_server_addresses(servers);
}

void StunPort::PrepareAddress() {
  SendStunBindingRequests();
}

}  // namespace webrtc
