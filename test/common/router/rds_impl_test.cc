#include <chrono>
#include <string>

#include "common/config/filter_json.h"
#include "common/config/utility.h"
#include "common/http/message_impl.h"
#include "common/json/json_loader.h"
#include "common/router/rds_impl.h"

#include "test/mocks/init/mocks.h"
#include "test/mocks/local_info/mocks.h"
#include "test/mocks/thread_local/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/test_common/printers.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
using testing::InSequence;
using testing::Invoke;
using testing::Return;
using testing::ReturnRef;
using testing::_;

namespace Router {
namespace {

envoy::api::v2::filter::HttpConnectionManager
parseHttpConnectionManagerFromJson(const std::string& json_string) {
  envoy::api::v2::filter::HttpConnectionManager http_connection_manager;
  auto json_object_ptr = Json::Factory::loadFromString(json_string);
  Envoy::Config::FilterJson::translateHttpConnectionManager(*json_object_ptr,
                                                            http_connection_manager);
  return http_connection_manager;
}

class RdsImplTest : public testing::Test {
public:
  RdsImplTest() : request_(&cm_.async_client_) {}
  ~RdsImplTest() { tls_.shutdownThread(); }

  void setup() {
    const std::string config_json = R"EOF(
    {
      "rds": {
        "cluster": "foo_cluster",
        "route_config_name": "foo_route_config",
        "refresh_delay_ms": 1000
      },
      "codec_type": "auto",
      "stat_prefix": "foo",
      "filters": [
        { "type": "both", "name": "http_dynamo_filter", "config": {} }
      ]
    }
    )EOF";

    interval_timer_ = new Event::MockTimer(&dispatcher_);
    EXPECT_CALL(init_manager_, registerTarget(_));
    rds_ = RouteConfigProviderUtil::create(parseHttpConnectionManagerFromJson(config_json),
                                           runtime_, cm_, store_, "foo.", init_manager_,
                                           route_config_provider_manager_);
    expectRequest();
    init_manager_.initialize();
  }

  void expectRequest() {
    EXPECT_CALL(cm_, httpAsyncClientForCluster("foo_cluster"));
    EXPECT_CALL(cm_.async_client_, send_(_, _, _))
        .WillOnce(
            Invoke([&](Http::MessagePtr& request, Http::AsyncClient::Callbacks& callbacks,
                       const Optional<std::chrono::milliseconds>&) -> Http::AsyncClient::Request* {
              EXPECT_EQ((Http::TestHeaderMapImpl{
                            {":method", "GET"},
                            {":path", "/v1/routes/foo_route_config/cluster_name/node_name"},
                            {":authority", "foo_cluster"}}),
                        request->headers());
              callbacks_ = &callbacks;
              return &request_;
            }));
  }

  NiceMock<Runtime::MockLoader> runtime_;
  NiceMock<Upstream::MockClusterManager> cm_;
  Event::MockDispatcher dispatcher_;
  NiceMock<Runtime::MockRandomGenerator> random_;
  NiceMock<LocalInfo::MockLocalInfo> local_info_;
  Stats::IsolatedStoreImpl store_;
  NiceMock<ThreadLocal::MockInstance> tls_;
  NiceMock<Init::MockManager> init_manager_;
  Http::MockAsyncClientRequest request_;
  RouteConfigProviderManagerImpl route_config_provider_manager_{runtime_, dispatcher_, random_,
                                                                local_info_, tls_};
  RouteConfigProviderSharedPtr rds_;
  Event::MockTimer* interval_timer_{};
  Http::AsyncClient::Callbacks* callbacks_{};
};

TEST_F(RdsImplTest, RdsAndStatic) {
  const std::string config_json = R"EOF(
    {
      "rds": {},
      "route_config": {},
      "codec_type": "auto",
      "stat_prefix": "foo",
      "filters": [
        { "type": "both", "name": "http_dynamo_filter", "config": {} }
      ]
    }
    )EOF";

  EXPECT_THROW(RouteConfigProviderUtil::create(parseHttpConnectionManagerFromJson(config_json),
                                               runtime_, cm_, store_, "foo.", init_manager_,
                                               route_config_provider_manager_),
               EnvoyException);
}

TEST_F(RdsImplTest, LocalInfoNotDefined) {
  const std::string config_json = R"EOF(
    {
      "rds": {
        "cluster": "foo_cluster",
        "route_config_name": "foo_route_config"
      },
      "codec_type": "auto",
      "stat_prefix": "foo",
      "filters": [
        { "type": "both", "name": "http_dynamo_filter", "config": {} }
      ]
    }
    )EOF";

  local_info_.cluster_name_ = "";
  local_info_.node_name_ = "";
  EXPECT_THROW(RouteConfigProviderUtil::create(parseHttpConnectionManagerFromJson(config_json),
                                               runtime_, cm_, store_, "foo.", init_manager_,
                                               route_config_provider_manager_),
               EnvoyException);
}

TEST_F(RdsImplTest, UnknownCluster) {
  const std::string config_json = R"EOF(
    {
      "rds": {
        "cluster": "foo_cluster",
        "route_config_name": "foo_route_config"
      },
      "codec_type": "auto",
      "stat_prefix": "foo",
      "filters": [
        { "type": "both", "name": "http_dynamo_filter", "config": {} }
      ]
    }
    )EOF";

  EXPECT_CALL(cm_, get("foo_cluster")).WillOnce(Return(nullptr));
  interval_timer_ = new Event::MockTimer(&dispatcher_);
  EXPECT_THROW(dynamic_cast<RdsRouteConfigProviderImpl*>(
                   RouteConfigProviderUtil::create(parseHttpConnectionManagerFromJson(config_json),
                                                   runtime_, cm_, store_, "foo.", init_manager_,
                                                   route_config_provider_manager_)
                       .get())
                   ->initialize([] {}),
               EnvoyException);
}

TEST_F(RdsImplTest, DestroyDuringInitialize) {
  InSequence s;

  setup();
  EXPECT_CALL(init_manager_.initialized_, ready());
  EXPECT_CALL(request_, cancel());
  rds_.reset();
}

TEST_F(RdsImplTest, Basic) {
  InSequence s;

  setup();

  // Make sure the initial empty route table works.
  EXPECT_EQ(nullptr, rds_->config()->route(Http::TestHeaderMapImpl{{":authority", "foo"}}, 0));

  // Initial request.
  std::string response1_json = R"EOF(
  {
    "virtual_hosts": []
  }
  )EOF";

  Http::MessagePtr message(new Http::ResponseMessageImpl(
      Http::HeaderMapPtr{new Http::TestHeaderMapImpl{{":status", "200"}}}));
  message->body().reset(new Buffer::OwnedImpl(response1_json));

  EXPECT_CALL(init_manager_.initialized_, ready());
  EXPECT_CALL(*interval_timer_, enableTimer(_));
  callbacks_->onSuccess(std::move(message));
  EXPECT_EQ(nullptr, rds_->config()->route(Http::TestHeaderMapImpl{{":authority", "foo"}}, 0));

  expectRequest();
  interval_timer_->callback_();

  // 2nd request with same response. Based on hash should not reload config.
  message.reset(new Http::ResponseMessageImpl(
      Http::HeaderMapPtr{new Http::TestHeaderMapImpl{{":status", "200"}}}));
  message->body().reset(new Buffer::OwnedImpl(response1_json));

  EXPECT_CALL(*interval_timer_, enableTimer(_));
  callbacks_->onSuccess(std::move(message));
  EXPECT_EQ(nullptr, rds_->config()->route(Http::TestHeaderMapImpl{{":authority", "foo"}}, 0));

  expectRequest();
  interval_timer_->callback_();

  // Load the config and verified shared count.
  ConfigConstSharedPtr config = rds_->config();
  EXPECT_EQ(2, config.use_count());

  // Third request.
  std::string response2_json = R"EOF(
  {
    "virtual_hosts": [
    {
      "name": "local_service",
      "domains": ["*"],
      "routes": [
        {
          "prefix": "/foo",
          "cluster_header": ":authority"
        },
        {
          "prefix": "/bar",
          "cluster": "bar"
        }
      ]
    }
  ]
  }
  )EOF";

  message.reset(new Http::ResponseMessageImpl(
      Http::HeaderMapPtr{new Http::TestHeaderMapImpl{{":status", "200"}}}));
  message->body().reset(new Buffer::OwnedImpl(response2_json));

  // Make sure we don't lookup/verify clusters.
  EXPECT_CALL(cm_, get("bar")).Times(0);
  EXPECT_CALL(*interval_timer_, enableTimer(_));
  callbacks_->onSuccess(std::move(message));
  EXPECT_EQ("foo", rds_->config()
                       ->route(Http::TestHeaderMapImpl{{":authority", "foo"}, {":path", "/foo"}}, 0)
                       ->routeEntry()
                       ->clusterName());

  // Old config use count should be 1 now.
  EXPECT_EQ(1, config.use_count());

  EXPECT_EQ(2UL, store_.counter("foo.rds.config_reload").value());
  EXPECT_EQ(3UL, store_.counter("foo.rds.update_attempt").value());
  EXPECT_EQ(3UL, store_.counter("foo.rds.update_success").value());
}

TEST_F(RdsImplTest, Failure) {
  InSequence s;

  setup();

  std::string response_json = R"EOF(
  {
    "blah": true
  }
  )EOF";

  Http::MessagePtr message(new Http::ResponseMessageImpl(
      Http::HeaderMapPtr{new Http::TestHeaderMapImpl{{":status", "200"}}}));
  message->body().reset(new Buffer::OwnedImpl(response_json));

  EXPECT_CALL(init_manager_.initialized_, ready());
  EXPECT_CALL(*interval_timer_, enableTimer(_));
  callbacks_->onSuccess(std::move(message));

  expectRequest();
  interval_timer_->callback_();

  EXPECT_CALL(*interval_timer_, enableTimer(_));
  callbacks_->onFailure(Http::AsyncClient::FailureReason::Reset);

  EXPECT_EQ(2UL, store_.counter("foo.rds.update_attempt").value());
  EXPECT_EQ(2UL, store_.counter("foo.rds.update_failure").value());
}

TEST_F(RdsImplTest, FailureArray) {
  InSequence s;

  setup();

  std::string response_json = R"EOF(
  []
  )EOF";

  Http::MessagePtr message(new Http::ResponseMessageImpl(
      Http::HeaderMapPtr{new Http::TestHeaderMapImpl{{":status", "200"}}}));
  message->body().reset(new Buffer::OwnedImpl(response_json));

  EXPECT_CALL(init_manager_.initialized_, ready());
  EXPECT_CALL(*interval_timer_, enableTimer(_));
  callbacks_->onSuccess(std::move(message));

  EXPECT_EQ(1UL, store_.counter("foo.rds.update_attempt").value());
  EXPECT_EQ(1UL, store_.counter("foo.rds.update_failure").value());
}

class RouteConfigProviderManagerImplTest : public testing::Test {
public:
  NiceMock<Runtime::MockLoader> runtime_;
  NiceMock<Upstream::MockClusterManager> cm_;
  Event::MockDispatcher dispatcher_;
  NiceMock<Runtime::MockRandomGenerator> random_;
  NiceMock<LocalInfo::MockLocalInfo> local_info_;
  Stats::IsolatedStoreImpl store_;
  NiceMock<ThreadLocal::MockInstance> tls_;
  NiceMock<Init::MockManager> init_manager_;
  RouteConfigProviderManagerImpl route_config_provider_manager_{runtime_, dispatcher_, random_,
                                                                local_info_, tls_};
};

TEST_F(RouteConfigProviderManagerImplTest, Basic) {
  init_manager_.initialize();

  std::string config_json = R"EOF(
    {
      "cluster": "foo_cluster",
      "route_config_name": "foo_route_config",
      "refresh_delay_ms": 1000
    }
    )EOF";

  Json::ObjectSharedPtr config = Json::Factory::loadFromString(config_json);
  envoy::api::v2::filter::Rds rds;
  Envoy::Config::Utility::translateRdsConfig(*config, rds);

  // Get a RouteConfigProvider. This one should create an entry in the RouteConfigProviderManager.
  RouteConfigProviderSharedPtr provider = route_config_provider_manager_.getRouteConfigProvider(
      rds, cm_, store_, "foo_prefix", init_manager_);
  // Because this get has the same cluster and route_config_name, the provider returned is just a
  // shared_ptr to the same provider as the one above.
  RouteConfigProviderSharedPtr provider2 = route_config_provider_manager_.getRouteConfigProvider(
      rds, cm_, store_, "foo_prefix", init_manager_);
  // So this means that both shared_ptrs should be the same.
  EXPECT_EQ(provider, provider2);
  EXPECT_EQ(2UL, provider.use_count());

  std::string config_json2 = R"EOF(
    {
      "cluster": "bar_cluster",
      "route_config_name": "foo_route_config",
      "refresh_delay_ms": 1000
    }
    )EOF";

  Json::ObjectSharedPtr config2 = Json::Factory::loadFromString(config_json2);
  envoy::api::v2::filter::Rds rds2;
  Envoy::Config::Utility::translateRdsConfig(*config2, rds2);

  RouteConfigProviderSharedPtr provider3 = route_config_provider_manager_.getRouteConfigProvider(
      rds2, cm_, store_, "foo_prefix", init_manager_);
  EXPECT_NE(provider3, provider);
  EXPECT_EQ(2UL, provider.use_count());
  EXPECT_EQ(1UL, provider3.use_count());

  std::vector<RouteConfigProviderSharedPtr> configured_providers =
      route_config_provider_manager_.routeConfigProviders();
  EXPECT_EQ(2UL, configured_providers.size());
  EXPECT_EQ(3UL, provider.use_count());
  EXPECT_EQ(2UL, provider3.use_count());

  provider.reset();
  provider2.reset();
  configured_providers.clear();

  // All shared_ptrs to the provider pointed at by provider1, and provider2 have been deleted, so
  // now we should only have the provider pointed at by provider3.
  configured_providers = route_config_provider_manager_.routeConfigProviders();
  EXPECT_EQ(1UL, configured_providers.size());
  EXPECT_EQ(provider3, configured_providers.front());

  provider3.reset();
  configured_providers.clear();

  configured_providers = route_config_provider_manager_.routeConfigProviders();
  EXPECT_EQ(0UL, configured_providers.size());
}

} // namespace
} // namespace Router
} // namespace Envoy
