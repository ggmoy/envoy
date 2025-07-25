#include "source/extensions/load_balancing_policies/common/thread_aware_lb_impl.h"

#include <memory>
#include <random>

#include "source/common/common/hex.h"
#include "source/common/http/headers.h"
#include "source/common/http/utility.h"

namespace Envoy {
namespace Upstream {

// TODO(mergeconflict): Adjust locality weights for partial availability, as is done in
//                      HostSetImpl::effectiveLocalityWeight.
namespace {

absl::Status normalizeHostWeights(const HostVector& hosts, double normalized_locality_weight,
                                  NormalizedHostWeightVector& normalized_host_weights,
                                  double& min_normalized_weight, double& max_normalized_weight) {
  // sum should be at most uint32_t max value, so we can validate it by accumulating into unit64_t
  // and making sure there was no overflow
  uint64_t sum = 0;
  for (const auto& host : hosts) {
    sum += host->weight();
    if (sum > std::numeric_limits<uint32_t>::max()) {
      return absl::InvalidArgumentError(
          fmt::format("The sum of weights of all upstream hosts in a locality exceeds {}",
                      std::numeric_limits<uint32_t>::max()));
    }
  }

  for (const auto& host : hosts) {
    const double weight = host->weight() * normalized_locality_weight / sum;
    normalized_host_weights.push_back({host, weight});
    min_normalized_weight = std::min(min_normalized_weight, weight);
    max_normalized_weight = std::max(max_normalized_weight, weight);
  }
  return absl::OkStatus();
}

absl::Status normalizeLocalityWeights(const HostsPerLocality& hosts_per_locality,
                                      const LocalityWeights& locality_weights,
                                      NormalizedHostWeightVector& normalized_host_weights,
                                      double& min_normalized_weight,
                                      double& max_normalized_weight) {
  ASSERT(locality_weights.size() == hosts_per_locality.get().size());

  // sum should be at most uint32_t max value, so we can validate it by accumulating into unit64_t
  // and making sure there was no overflow
  uint64_t sum = 0;
  for (const auto weight : locality_weights) {
    sum += weight;
    if (sum > std::numeric_limits<uint32_t>::max()) {
      return absl::InvalidArgumentError(
          fmt::format("The sum of weights of all localities at the same priority exceeds {}",
                      std::numeric_limits<uint32_t>::max()));
    }
  }

  // Locality weights (unlike host weights) may be 0. If _all_ locality weights were 0, bail out.
  if (sum == 0) {
    return absl::OkStatus();
  }

  // Compute normalized weights for all hosts in each locality. If a locality was assigned zero
  // weight, all hosts in that locality will be skipped.
  for (LocalityWeights::size_type i = 0; i < locality_weights.size(); ++i) {
    if (locality_weights[i] != 0) {
      const HostVector& hosts = hosts_per_locality.get()[i];
      const double normalized_locality_weight = static_cast<double>(locality_weights[i]) / sum;
      RETURN_IF_NOT_OK(normalizeHostWeights(hosts, normalized_locality_weight,
                                            normalized_host_weights, min_normalized_weight,
                                            max_normalized_weight));
    }
  }
  return absl::OkStatus();
}

absl::Status normalizeWeights(const HostSet& host_set, bool in_panic,
                              NormalizedHostWeightVector& normalized_host_weights,
                              double& min_normalized_weight, double& max_normalized_weight,
                              bool locality_weighted_balancing) {
  if (!locality_weighted_balancing || host_set.localityWeights() == nullptr ||
      host_set.localityWeights()->empty()) {
    // If we're not dealing with locality weights, just normalize weights for the flat set of hosts.
    const auto& hosts = in_panic ? host_set.hosts() : host_set.healthyHosts();
    RETURN_IF_NOT_OK(normalizeHostWeights(hosts, 1.0, normalized_host_weights,
                                          min_normalized_weight, max_normalized_weight));
  } else {
    // Otherwise, normalize weights across all localities.
    const auto& hosts_per_locality =
        in_panic ? host_set.hostsPerLocality() : host_set.healthyHostsPerLocality();
    RETURN_IF_NOT_OK(normalizeLocalityWeights(hosts_per_locality, *(host_set.localityWeights()),
                                              normalized_host_weights, min_normalized_weight,
                                              max_normalized_weight));
  }
  return absl::OkStatus();
}

std::string generateCookie(LoadBalancerContext* context, absl::string_view name,
                           absl::string_view path, std::chrono::seconds ttl,
                           absl::Span<const Http::CookieAttribute> attributes) {
  ASSERT(context != nullptr);
  const StreamInfo::StreamInfo* stream_info = context->requestStreamInfo();
  if (stream_info == nullptr) {
    return {};
  }

  const auto& conn = stream_info->downstreamAddressProvider();
  const auto& remote_address = conn.remoteAddress();
  const auto& local_address = conn.localAddress();
  if (remote_address == nullptr || local_address == nullptr) {
    return {};
  }

  const std::string value = remote_address->asString() + local_address->asString();
  std::string cookie_value = Hex::uint64ToHex(HashUtil::xxHash64(value));

  std::string cookie_header_value =
      Http::Utility::makeSetCookieValue(name, cookie_value, path, ttl, true, attributes);
  context->setHeadersModifier(
      [h = std::move(cookie_header_value)](Http::ResponseHeaderMap& headers) {
        headers.addReferenceKey(Http::Headers::get().SetCookie, h);
      });

  return cookie_value;
}

} // namespace

absl::Status ThreadAwareLoadBalancerBase::initialize() {
  // TODO(mattklein123): In the future, once initialized and the initial LB is built, it would be
  // better to use a background thread for computing LB updates. This has the substantial benefit
  // that if the LB computation thread falls behind, host set updates can be trivially collapsed.
  // I will look into doing this in a follow up. Doing everything using a background thread heavily
  // complicated initialization as the load balancer would need its own initialized callback. I
  // think the synchronous/asynchronous split is probably the best option.
  priority_update_cb_ = priority_set_.addPriorityUpdateCb(
      [this](uint32_t, const HostVector&, const HostVector&) -> absl::Status { return refresh(); });

  return refresh();
}

absl::Status ThreadAwareLoadBalancerBase::refresh() {
  auto per_priority_state_vector = std::make_shared<std::vector<PerPriorityStatePtr>>(
      priority_set_.hostSetsPerPriority().size());
  auto healthy_per_priority_load =
      std::make_shared<HealthyLoad>(per_priority_load_.healthy_priority_load_);
  auto degraded_per_priority_load =
      std::make_shared<DegradedLoad>(per_priority_load_.degraded_priority_load_);

  for (const auto& host_set : priority_set_.hostSetsPerPriority()) {
    const uint32_t priority = host_set->priority();
    (*per_priority_state_vector)[priority] = std::make_unique<PerPriorityState>();
    const auto& per_priority_state = (*per_priority_state_vector)[priority];
    // Copy panic flag from LoadBalancerBase. It is calculated when there is a change
    // in hosts set or hosts' health.
    per_priority_state->global_panic_ = per_priority_panic_[priority];

    // Normalize host and locality weights such that the sum of all normalized weights is 1.
    NormalizedHostWeightVector normalized_host_weights;
    double min_normalized_weight = 1.0;
    double max_normalized_weight = 0.0;
    absl::Status status = normalizeWeights(*host_set, per_priority_state->global_panic_,
                                           normalized_host_weights, min_normalized_weight,
                                           max_normalized_weight, locality_weighted_balancing_);
    RETURN_IF_NOT_OK(status);
    per_priority_state->current_lb_ = createLoadBalancer(
        std::move(normalized_host_weights), min_normalized_weight, max_normalized_weight);
  }

  {
    absl::WriterMutexLock lock(&factory_->mutex_);
    factory_->healthy_per_priority_load_ = healthy_per_priority_load;
    factory_->degraded_per_priority_load_ = degraded_per_priority_load;
    factory_->per_priority_state_ = per_priority_state_vector;
  }
  return absl::OkStatus();
}

HostSelectionResponse
ThreadAwareLoadBalancerBase::LoadBalancerImpl::chooseHost(LoadBalancerContext* context) {
  // Make sure we correctly return nullptr for any early chooseHost() calls.
  if (per_priority_state_ == nullptr) {
    return {nullptr};
  }

  HostConstSharedPtr host;

  // If there is no hash in the context, just choose a random value (this effectively becomes
  // the random LB but it won't crash if someone configures it this way).
  // computeHashKey() may be computed on demand, so get it only once.
  absl::optional<uint64_t> hash;
  if (context) {
    // If there is a hash policy, use the hash policy in the load balancer first.
    if (hash_policy_ != nullptr) {
      hash = hash_policy_->generateHash(
          makeOptRefFromPtr(context->downstreamHeaders()),
          makeOptRefFromPtr(context->requestStreamInfo()),
          [context](absl::string_view name, absl::string_view path, std::chrono::seconds ttl,
                    absl::Span<const Http::CookieAttribute> attributes) -> std::string {
            return generateCookie(context, name, path, ttl, attributes);
          });
    } else {
      hash = context->computeHashKey();
    }
  }

  const uint64_t h = hash ? hash.value() : random_.random();

  const uint32_t priority =
      LoadBalancerBase::choosePriority(h, *healthy_per_priority_load_, *degraded_per_priority_load_)
          .first;
  const auto& per_priority_state = (*per_priority_state_)[priority];
  if (per_priority_state->global_panic_) {
    stats_.lb_healthy_panic_.inc();
  }

  const uint32_t max_attempts = context ? context->hostSelectionRetryCount() + 1 : 1;
  for (uint32_t i = 0; i < max_attempts; ++i) {
    host = LoadBalancer::onlyAllowSynchronousHostSelection(
        per_priority_state->current_lb_->chooseHost(h, i));

    // If host selection failed or the host is accepted by the filter, return.
    // Otherwise, try again.
    if (!host || !context || !context->shouldSelectAnotherHost(*host)) {
      return host;
    }
  }
  return host;
}

LoadBalancerPtr ThreadAwareLoadBalancerBase::LoadBalancerFactoryImpl::create(LoadBalancerParams) {
  auto lb = std::make_unique<LoadBalancerImpl>(stats_, random_, hash_policy_);

  // We must protect current_lb_ via a RW lock since it is accessed and written to by multiple
  // threads. All complex processing has already been precalculated however.
  absl::ReaderMutexLock lock(&mutex_);
  lb->healthy_per_priority_load_ = healthy_per_priority_load_;
  lb->degraded_per_priority_load_ = degraded_per_priority_load_;
  lb->per_priority_state_ = per_priority_state_;
  return lb;
}

double ThreadAwareLoadBalancerBase::BoundedLoadHashingLoadBalancer::hostOverloadFactor(
    const Host& host, double weight) const {
  // TODO(scheler): This will not work if rq_active cluster stat is disabled, need to detect
  // and alert the user if that's the case.

  const uint32_t overall_active = host.cluster().trafficStats()->upstream_rq_active_.value();
  const uint32_t host_active = host.stats().rq_active_.value();

  const uint32_t total_slots = ((overall_active + 1) * hash_balance_factor_ + 99) / 100;
  const uint32_t slots =
      std::max(static_cast<uint32_t>(std::ceil(total_slots * weight)), static_cast<uint32_t>(1));

  if (host.stats().rq_active_.value() > slots) {
    ENVOY_LOG_MISC(
        debug,
        "ThreadAwareLoadBalancerBase::BoundedLoadHashingLoadBalancer::chooseHost: "
        "host {} overloaded; overall_active {}, host_weight {}, host_active {} > slots {}",
        host.address()->asString(), overall_active, weight, host_active, slots);
  }
  return static_cast<double>(host.stats().rq_active_.value()) / slots;
}

HostSelectionResponse
ThreadAwareLoadBalancerBase::BoundedLoadHashingLoadBalancer::chooseHost(uint64_t hash,
                                                                        uint32_t attempt) const {

  // This is implemented based on the method described in the paper
  // https://arxiv.org/abs/1608.01350. For the specified `hash_balance_factor`, requests to any
  // upstream host are capped at `hash_balance_factor/100` times the average number of requests
  // across the cluster. When a request arrives for an upstream host that is currently serving at
  // its max capacity, linear probing is used to identify an eligible host. Further, the linear
  // probe is implemented using a random jump on hosts ring/table to identify the eligible host
  // (this technique is as described in the paper https://arxiv.org/abs/1908.08762 - the random jump
  // avoids the cascading overflow effect when choosing the next host on the ring/table).
  //
  // If weights are specified on the hosts, they are respected.
  //
  // This is an O(N) algorithm, unlike other load balancers. Using a lower `hash_balance_factor`
  // results in more hosts being probed, so use a higher value if you require better performance.

  if (normalized_host_weights_.empty()) {
    return {nullptr};
  }

  HostConstSharedPtr host =
      LoadBalancer::onlyAllowSynchronousHostSelection(hashing_lb_ptr_->chooseHost(hash, attempt));
  if (host == nullptr) {
    return {nullptr};
  }
  const double weight = normalized_host_weights_map_.at(host);
  double overload_factor = hostOverloadFactor(*host, weight);
  if (overload_factor <= 1.0) {
    ENVOY_LOG_MISC(debug,
                   "ThreadAwareLoadBalancerBase::BoundedLoadHashingLoadBalancer::chooseHost: "
                   "selected host #{} (attempt:1)",
                   host->address()->asString());
    return host;
  }

  // When a host is overloaded, we choose the next host in a random manner rather than picking the
  // next one in the ring. The random sequence is seeded by the hash, so the same input gets the
  // same sequence of hosts all the time.
  const uint32_t num_hosts = normalized_host_weights_.size();
  auto host_index = std::vector<uint32_t>(num_hosts);
  for (uint32_t i = 0; i < num_hosts; i++) {
    host_index[i] = i;
  }

  // Not using Random::RandomGenerator as it does not take a seed. Seeded RNG is a requirement
  // here as we need the same shuffle sequence for the same hash every time.
  // Further, not using std::default_random_engine and std::uniform_int_distribution as they
  // are not consistent across Linux and Windows platforms.
  const uint64_t seed = hash;
  std::mt19937 random(seed);

  // generates a random number in the range [0,k) uniformly.
  auto uniform_int = [](std::mt19937& random, uint32_t k) -> uint32_t {
    uint32_t x = k;
    while (x >= k) {
      x = random() / ((static_cast<uint64_t>(random.max()) + 1u) / k);
    }
    return x;
  };

  HostConstSharedPtr alt_host, least_overloaded_host = host;
  double least_overload_factor = overload_factor;
  for (uint32_t i = 0; i < num_hosts; i++) {
    // The random shuffle algorithm
    const uint32_t j = uniform_int(random, num_hosts - i);
    std::swap(host_index[i], host_index[i + j]);

    const uint32_t k = host_index[i];
    alt_host = normalized_host_weights_[k].first;
    if (alt_host == host) {
      continue;
    }

    const double alt_host_weight = normalized_host_weights_[k].second;
    overload_factor = hostOverloadFactor(*alt_host, alt_host_weight);

    if (overload_factor <= 1.0) {
      ENVOY_LOG_MISC(debug,
                     "ThreadAwareLoadBalancerBase::BoundedLoadHashingLoadBalancer::chooseHost: "
                     "selected host #{}:{} (attempt:{})",
                     k, alt_host->address()->asString(), i + 2);
      return alt_host;
    }

    if (least_overload_factor > overload_factor) {
      least_overloaded_host = alt_host;
      least_overload_factor = overload_factor;
    }
  }

  return least_overloaded_host;
}

TypedHashLbConfigBase::TypedHashLbConfigBase(absl::Span<const HashPolicyProto* const> hash_policy,
                                             Regex::Engine& regex_engine,
                                             absl::Status& creation_status) {
  if (hash_policy.empty()) {
    return;
  }
  auto hash_policy_or = Http::HashPolicyImpl::create(hash_policy, regex_engine);
  SET_AND_RETURN_IF_NOT_OK(hash_policy_or.status(), creation_status);
  hash_policy_ = std::move(hash_policy_or).value();
}

} // namespace Upstream
} // namespace Envoy
