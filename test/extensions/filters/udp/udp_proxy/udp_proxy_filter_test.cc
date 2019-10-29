#include "envoy/config/filter/udp/udp_proxy/v2alpha/udp_proxy.pb.validate.h"

#include "extensions/filters/udp/udp_proxy/udp_proxy_filter.h"

#include "test/mocks/network/io_handle.h"
#include "test/mocks/upstream/mocks.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::AtLeast;
using testing::ByMove;
using testing::InSequence;
using testing::Return;
using testing::SaveArg;

namespace Envoy {
namespace Extensions {
namespace UdpFilters {
namespace UdpProxy {
namespace {

class TestUdpProxyFilter : public UdpProxyFilter {
public:
  using UdpProxyFilter::UdpProxyFilter;

  MOCK_METHOD1(createIoHandle, Network::IoHandlePtr(const Upstream::HostConstSharedPtr& host));
};

Api::IoCallUint64Result makeNoError(uint64_t rc) {
  auto no_error = Api::ioCallUint64ResultNoError();
  no_error.rc_ = rc;
  return no_error;
}

class UdpProxyFilterTest : public testing::Test {
public:
  struct TestSession {
    TestSession(UdpProxyFilterTest& parent,
                const Network::Address::InstanceConstSharedPtr& upstream_address)
        : parent_(parent), upstream_address_(upstream_address),
          io_handle_(new Network::MockIoHandle()) {}

    void expectUpstreamWrite(const std::string& data) {
      EXPECT_CALL(*io_handle_, sendmsg(_, 1, 0, nullptr, _))
          .WillOnce(Invoke(
              [this,
               data](const Buffer::RawSlice* slices, uint64_t, int, const Network::Address::Ip*,
                     const Network::Address::Instance& peer_address) -> Api::IoCallUint64Result {
                EXPECT_EQ(data, absl::string_view(static_cast<const char*>(slices[0].mem_),
                                                  slices[0].len_));
                EXPECT_EQ(peer_address, *upstream_address_);
                return makeNoError(data.size());
              }));
    }

    void recvData(const std::string& data) {
      // Return the datagram.
      EXPECT_CALL(*io_handle_, recvmsg(_, 1, _, _))
          .WillOnce(Invoke(
              [this, data](Buffer::RawSlice* slices, const uint64_t, uint32_t,
                           Network::IoHandle::RecvMsgOutput& output) -> Api::IoCallUint64Result {
                ASSERT(data.size() <= slices[0].len_);
                memcpy(slices[0].mem_, data.data(), data.size());
                output.peer_address_ = upstream_address_;
                return makeNoError(data.size());
                ;
              }));
      // Send the datagram downstream.
      EXPECT_CALL(parent_.callbacks_.udp_listener_, send(_))
          .WillOnce(
              Invoke([data](const Network::UdpSendData& send_data) -> Api::IoCallUint64Result {
                // fixfix verify peer/local address.
                EXPECT_EQ(send_data.buffer_.toString(), data);
                return makeNoError(data.size());
              }));
      // Return an EAGAIN result.
      EXPECT_CALL(*io_handle_, recvmsg(_, 1, _, _))
          .WillOnce(Return(ByMove(Api::IoCallUint64Result(
              0, Api::IoErrorPtr(Network::IoSocketError::getIoSocketEagainInstance(),
                                 Network::IoSocketError::deleteIoError)))));
      // Kick off the receive.
      file_event_cb_(Event::FileReadyType::Read);
    }

    UdpProxyFilterTest& parent_;
    const Network::Address::InstanceConstSharedPtr upstream_address_;
    Event::MockTimer* idle_timer_{};
    Network::MockIoHandle* io_handle_;
    Event::FileReadyCb file_event_cb_;
  };

  UdpProxyFilterTest()
      : upstream_address_(Network::Utility::parseInternetAddressAndPort("20.0.0.1:443")) {
    // Disable strict mock warnings.
    EXPECT_CALL(callbacks_, udpListener()).Times(AtLeast(0));
    EXPECT_CALL(*cluster_manager_.thread_local_cluster_.lb_.host_, address())
        .WillRepeatedly(Return(upstream_address_));
  }

  ~UdpProxyFilterTest() { EXPECT_CALL(callbacks_.udp_listener_, onDestroy()); }

  void setup(const std::string& yaml) {
    envoy::config::filter::udp::udp_proxy::v2alpha::UdpProxyConfig config;
    TestUtility::loadFromYamlAndValidate(yaml, config);
    config_ = std::make_shared<UdpProxyFilterConfig>(cluster_manager_, time_system_, stats_store_,
                                                     config);
    filter_ = std::make_unique<TestUdpProxyFilter>(callbacks_, config_);
  }

  void recvData(const std::string& peer_address, const std::string& local_address,
                const std::string& buffer) {
    Network::UdpRecvData data;
    data.addresses_.peer_ = Network::Utility::parseInternetAddressAndPort(peer_address);
    data.addresses_.local_ = Network::Utility::parseInternetAddressAndPort(local_address);
    data.buffer_ = std::make_unique<Buffer::OwnedImpl>(buffer);
    data.receive_time_ = MonotonicTime(std::chrono::seconds(0));
    filter_->onData(data);
  }

  void expectSessionCreate() {
    test_sessions_.emplace_back(*this, upstream_address_);
    TestSession& new_session = test_sessions_.back();
    EXPECT_CALL(cluster_manager_, get(_));
    new_session.idle_timer_ = new Event::MockTimer(&callbacks_.udp_listener_.dispatcher_);
    EXPECT_CALL(*filter_, createIoHandle(_))
        .WillOnce(Return(ByMove(Network::IoHandlePtr{test_sessions_.back().io_handle_})));
    EXPECT_CALL(*new_session.io_handle_, fd());
    EXPECT_CALL(callbacks_.udp_listener_.dispatcher_,
                createFileEvent_(_, _, Event::FileTriggerType::Edge, Event::FileReadyType::Read))
        .WillOnce(DoAll(SaveArg<1>(&new_session.file_event_cb_), Return(nullptr)));
  }

  Upstream::MockClusterManager cluster_manager_;
  NiceMock<MockTimeSystem> time_system_;
  Stats::IsolatedStoreImpl stats_store_;
  UdpProxyFilterConfigSharedPtr config_;
  Network::MockUdpReadFilterCallbacks callbacks_;
  std::unique_ptr<TestUdpProxyFilter> filter_;
  std::vector<TestSession> test_sessions_;
  // If a test ever support more than 1 upstream host this will need to move to the session/test.
  const Network::Address::InstanceConstSharedPtr upstream_address_;
};

// fixfix
TEST_F(UdpProxyFilterTest, BasicFlow) {
  InSequence s;

  setup(R"EOF(
cluster: fake_cluster
  )EOF");

  expectSessionCreate();
  test_sessions_[0].expectUpstreamWrite("hello");
  recvData("10.0.0.1:1000", "10.0.0.2:80", "hello");

  test_sessions_[0].recvData("world");

  test_sessions_[0].expectUpstreamWrite("hello2");
  test_sessions_[0].expectUpstreamWrite("hello3");
  recvData("10.0.0.1:1000", "10.0.0.2:80", "hello2");
  recvData("10.0.0.1:1000", "10.0.0.2:80", "hello3");

  test_sessions_[0].recvData("world2");
  test_sessions_[0].recvData("world3");
}

} // namespace
} // namespace UdpProxy
} // namespace UdpFilters
} // namespace Extensions
} // namespace Envoy
