#include "fdbrpc/FlowTransport.h"
#include "fdbrpc/TokenCache.h"
#include "fdbrpc/TokenSign.h"
#include "fdbrpc/TenantInfo.h"
#include "flow/MkCert.h"
#include "flow/ScopeExit.h"
#include "flow/UnitTest.h"
#include "flow/network.h"

#include <boost/unordered_map.hpp>

#include <fmt/format.h>
#include <list>
#include <deque>

#include "flow/actorcompiler.h" // has to be last include

template <class Key, class Value>
class LRUCache {
public:
	using key_type = Key;
	using list_type = std::list<key_type>;
	using mapped_type = Value;
	using map_type = boost::unordered_map<key_type, std::pair<mapped_type, typename list_type::iterator>>;
	using size_type = unsigned;

	explicit LRUCache(size_type capacity) : _capacity(capacity) { _map.reserve(capacity); }

	size_type size() const { return _map.size(); }
	size_type capacity() const { return _capacity; }
	bool empty() const { return _map.empty(); }

	Optional<mapped_type*> get(key_type const& key) {
		auto i = _map.find(key);
		if (i == _map.end()) {
			return Optional<mapped_type*>();
		}
		auto j = i->second.second;
		if (j != _list.begin()) {
			_list.erase(j);
			_list.push_front(i->first);
			i->second.second = _list.begin();
		}
		return &i->second.first;
	}

	template <class K, class V>
	mapped_type* insert(K&& key, V&& value) {
		auto iter = _map.find(key);
		if (iter != _map.end()) {
			return &iter->second.first;
		}
		if (size() == capacity()) {
			auto i = --_list.end();
			_map.erase(*i);
			_list.erase(i);
		}
		_list.push_front(std::forward<K>(key));
		std::tie(iter, std::ignore) =
		    _map.insert(std::make_pair(*_list.begin(), std::make_pair(std::forward<V>(value), _list.begin())));
		return &iter->second.first;
	}

private:
	const size_type _capacity;
	map_type _map;
	list_type _list;
};

TEST_CASE("/fdbrpc/authz/LRUCache") {
	auto& rng = *deterministicRandom();
	{
		// test very small LRU cache
		LRUCache<int, StringRef> cache(rng.randomInt(2, 10));
		for (int i = 0; i < 200; ++i) {
			cache.insert(i, "val"_sr);
			if (i >= cache.capacity()) {
				for (auto j = 0; j <= i - cache.capacity(); j++)
					ASSERT(!cache.get(j).present());
				// ordering is important so as not to disrupt the LRU order
				for (auto j = i - cache.capacity() + 1; j <= i; j++)
					ASSERT(cache.get(j).present());
			}
		}
	}
	{
		// Test larger cache
		LRUCache<int, StringRef> cache(1000);
		for (auto i = 0; i < 1000; ++i) {
			cache.insert(i, "value"_sr);
		}
		cache.insert(1000, "value"_sr); // should evict 0
		ASSERT(!cache.get(0).present());
	}
	{
		// memory test -- this is what the boost implementation didn't do correctly
		LRUCache<StringRef, Standalone<StringRef>> cache(10);
		std::deque<std::string> cachedStrings;
		std::deque<std::string> evictedStrings;
		for (int i = 0; i < 10; ++i) {
			auto str = rng.randomAlphaNumeric(rng.randomInt(100, 1024));
			Standalone<StringRef> sref(str);
			cache.insert(sref, sref);
			cachedStrings.push_back(str);
		}
		for (int i = 0; i < 10; ++i) {
			Standalone<StringRef> existingStr(cachedStrings.back());
			auto cachedStr = cache.get(existingStr);
			ASSERT(cachedStr.present());
			ASSERT(*cachedStr.get() == existingStr);
			if (!evictedStrings.empty()) {
				Standalone<StringRef> nonexisting(evictedStrings.at(rng.randomInt(0, evictedStrings.size())));
				ASSERT(!cache.get(nonexisting).present());
			}
			auto str = rng.randomAlphaNumeric(rng.randomInt(100, 1024));
			Standalone<StringRef> sref(str);
			evictedStrings.push_back(cachedStrings.front());
			cachedStrings.pop_front();
			cachedStrings.push_back(str);
			cache.insert(sref, sref);
		}
	}
	return Void();
}

struct TokenCacheImpl {
	struct CacheEntry {
		Arena arena;
		VectorRef<TenantNameRef> tenants;
		double expirationTime = 0.0;
	};

	LRUCache<StringRef, CacheEntry> cache;
	TokenCacheImpl() : cache(FLOW_KNOBS->TOKEN_CACHE_SIZE) {}

	bool validate(TenantNameRef tenant, StringRef token);
	bool validateAndAdd(double currentTime, StringRef token, NetworkAddress const& peer);
};

TokenCache::TokenCache() : impl(new TokenCacheImpl()) {}
TokenCache::~TokenCache() {
	delete impl;
}

void TokenCache::createInstance() {
	g_network->setGlobal(INetwork::enTokenCache, new TokenCache());
}

TokenCache& TokenCache::instance() {
	return *reinterpret_cast<TokenCache*>(g_network->global(INetwork::enTokenCache));
}

bool TokenCache::validate(TenantNameRef name, StringRef token) {
	return impl->validate(name, token);
}

#define TRACE_INVALID_PARSED_TOKEN(reason, token)                                                                      \
	TraceEvent(SevWarn, "InvalidToken")                                                                                \
	    .detail("From", peer)                                                                                          \
	    .detail("Reason", reason)                                                                                      \
	    .detail("CurrentTime", currentTime)                                                                            \
	    .detail("Token", token.toStringRef(arena).toStringView())

bool TokenCacheImpl::validateAndAdd(double currentTime, StringRef token, NetworkAddress const& peer) {
	Arena arena;
	authz::jwt::TokenRef t;
	if (!authz::jwt::parseToken(arena, t, token)) {
		CODE_PROBE(true, "Token can't be parsed");
		TraceEvent(SevWarn, "InvalidToken")
		    .detail("From", peer)
		    .detail("Reason", "ParseError")
		    .detail("Token", token.toString());
		return false;
	}
	auto key = FlowTransport::transport().getPublicKeyByName(t.keyId);
	if (!key.present()) {
		CODE_PROBE(true, "Token referencing non-existing key");
		TRACE_INVALID_PARSED_TOKEN("UnknownKey", t);
		return false;
	} else if (!t.issuedAtUnixTime.present()) {
		CODE_PROBE(true, "Token has no issued-at field");
		TRACE_INVALID_PARSED_TOKEN("NoIssuedAt", t);
		return false;
	} else if (!t.expiresAtUnixTime.present()) {
		CODE_PROBE(true, "Token has no expiration time");
		TRACE_INVALID_PARSED_TOKEN("NoExpirationTime", t);
		return false;
	} else if (double(t.expiresAtUnixTime.get()) <= currentTime) {
		CODE_PROBE(true, "Expired token");
		TRACE_INVALID_PARSED_TOKEN("Expired", t);
		return false;
	} else if (!t.notBeforeUnixTime.present()) {
		CODE_PROBE(true, "Token has no not-before field");
		TRACE_INVALID_PARSED_TOKEN("NoNotBefore", t);
		return false;
	} else if (double(t.notBeforeUnixTime.get()) > currentTime) {
		CODE_PROBE(true, "Tokens not-before is in the future");
		TRACE_INVALID_PARSED_TOKEN("TokenNotYetValid", t);
		return false;
	} else if (!t.tenants.present()) {
		CODE_PROBE(true, "Token with no tenants");
		TRACE_INVALID_PARSED_TOKEN("NoTenants", t);
		return false;
	} else if (!authz::jwt::verifyToken(token, key.get())) {
		CODE_PROBE(true, "Token with invalid signature");
		TRACE_INVALID_PARSED_TOKEN("InvalidSignature", t);
		return false;
	} else {
		CacheEntry c;
		c.expirationTime = t.expiresAtUnixTime.get();
		c.tenants.reserve(c.arena, t.tenants.get().size());
		for (auto tenant : t.tenants.get()) {
			c.tenants.push_back_deep(c.arena, tenant);
		}
		cache.insert(StringRef(c.arena, token), c);
		return true;
	}
}

bool TokenCacheImpl::validate(TenantNameRef name, StringRef token) {
	NetworkAddress peer = FlowTransport::transport().currentDeliveryPeerAddress();
	auto cachedEntry = cache.get(token);
	double currentTime = g_network->timer();

	if (!cachedEntry.present()) {
		if (validateAndAdd(currentTime, token, peer)) {
			cachedEntry = cache.get(token);
		} else {
			return false;
		}
	}

	ASSERT(cachedEntry.present());

	auto& entry = cachedEntry.get();
	if (entry->expirationTime < currentTime) {
		CODE_PROBE(true, "Found expired token in cache");
		TraceEvent(SevWarn, "InvalidToken").detail("From", peer).detail("Reason", "ExpiredInCache");
		return false;
	}
	bool tenantFound = false;
	for (auto const& t : entry->tenants) {
		if (t == name) {
			tenantFound = true;
			break;
		}
	}
	if (!tenantFound) {
		CODE_PROBE(true, "Valid token doesn't reference tenant");
		TraceEvent(SevWarn, "TenantTokenMismatch").detail("From", peer).detail("Tenant", name.toString());
		return false;
	}
	return true;
}

namespace authz::jwt {
extern TokenRef makeRandomTokenSpec(Arena&, IRandom&, authz::Algorithm);
}

TEST_CASE("/fdbrpc/authz/TokenCache/BadTokens") {
	std::pair<void (*)(Arena&, IRandom&, authz::jwt::TokenRef&), char const*> badMutations[]{
		{
		    [](Arena&, IRandom&, authz::jwt::TokenRef&) { FlowTransport::transport().removeAllPublicKeys(); },
		    "NoKeyWithSuchName",
		},
		{
		    [](Arena&, IRandom&, authz::jwt::TokenRef& token) { token.expiresAtUnixTime.reset(); },
		    "NoExpirationTime",
		},
		{
		    [](Arena&, IRandom& rng, authz::jwt::TokenRef& token) {
		        token.expiresAtUnixTime = std::max<double>(g_network->timer() - 10 - rng.random01() * 50, 0);
		    },
		    "ExpiredToken",
		},
		{
		    [](Arena&, IRandom&, authz::jwt::TokenRef& token) { token.notBeforeUnixTime.reset(); },
		    "NoNotBefore",
		},
		{
		    [](Arena&, IRandom& rng, authz::jwt::TokenRef& token) {
		        token.notBeforeUnixTime = g_network->timer() + 10 + rng.random01() * 50;
		    },
		    "TokenNotYetValid",
		},
		{
		    [](Arena&, IRandom&, authz::jwt::TokenRef& token) { token.issuedAtUnixTime.reset(); },
		    "NoIssuedAt",
		},

		{
		    [](Arena& arena, IRandom&, authz::jwt::TokenRef& token) { token.tenants.reset(); },
		    "NoTenants",
		},
	};
	auto const pubKeyName = "somePublicKey"_sr;
	auto privateKey = mkcert::makeEcP256();
	auto const numBadMutations = sizeof(badMutations) / sizeof(badMutations[0]);
	for (auto repeat = 0; repeat < 50; repeat++) {
		auto arena = Arena();
		auto& rng = *deterministicRandom();
		auto validTokenSpec = authz::jwt::makeRandomTokenSpec(arena, rng, authz::Algorithm::ES256);
		validTokenSpec.keyId = pubKeyName;
		for (auto i = 0; i < numBadMutations; i++) {
			FlowTransport::transport().addPublicKey(pubKeyName, privateKey.toPublic());
			auto publicKeyClearGuard =
			    ScopeExit([pubKeyName]() { FlowTransport::transport().removePublicKey(pubKeyName); });
			auto [mutationFn, mutationDesc] = badMutations[i];
			auto tmpArena = Arena();
			auto mutatedTokenSpec = validTokenSpec;
			mutationFn(tmpArena, rng, mutatedTokenSpec);
			auto signedToken = authz::jwt::signToken(tmpArena, mutatedTokenSpec, privateKey);
			if (TokenCache::instance().validate(validTokenSpec.tenants.get()[0], signedToken)) {
				fmt::print("Unexpected successful validation at mutation {}, token spec: {}\n",
				           mutationDesc,
				           mutatedTokenSpec.toStringRef(tmpArena).toStringView());
				ASSERT(false);
			}
		}
	}
	if (TokenCache::instance().validate("TenantNameDontMatterHere"_sr, StringRef())) {
		fmt::print("Unexpected successful validation of ill-formed token (no signature part)\n");
		ASSERT(false);
	}
	if (TokenCache::instance().validate("TenantNameDontMatterHere"_sr, "1111.22"_sr)) {
		fmt::print("Unexpected successful validation of ill-formed token (no signature part)\n");
		ASSERT(false);
	}
	if (TokenCache::instance().validate("TenantNameDontMatterHere2"_sr, "////.////.////"_sr)) {
		fmt::print("Unexpected successful validation of unparseable token\n");
		ASSERT(false);
	}
	fmt::print("TEST OK\n");
	return Void();
}

TEST_CASE("/fdbrpc/authz/TokenCache/GoodTokens") {
	// Don't repeat because token expiry is at seconds granularity and sleeps are costly in unit tests
	state Arena arena;
	state PrivateKey privateKey = mkcert::makeEcP256();
	state StringRef pubKeyName = "somePublicKey"_sr;
	state ScopeExit<std::function<void()>> publicKeyClearGuard(
	    [pubKeyName = pubKeyName]() { FlowTransport::transport().removePublicKey(pubKeyName); });
	state authz::jwt::TokenRef tokenSpec =
	    authz::jwt::makeRandomTokenSpec(arena, *deterministicRandom(), authz::Algorithm::ES256);
	state StringRef signedToken;
	FlowTransport::transport().addPublicKey(pubKeyName, privateKey.toPublic());
	tokenSpec.expiresAtUnixTime = g_network->timer() + 2.0;
	tokenSpec.keyId = pubKeyName;
	signedToken = authz::jwt::signToken(arena, tokenSpec, privateKey);
	if (!TokenCache::instance().validate(tokenSpec.tenants.get()[0], signedToken)) {
		fmt::print("Unexpected failed token validation, token spec: {}, now: {}\n",
		           tokenSpec.toStringRef(arena).toStringView(),
		           g_network->timer());
		ASSERT(false);
	}
	wait(delay(3.5));
	if (TokenCache::instance().validate(tokenSpec.tenants.get()[0], signedToken)) {
		fmt::print(
		    "Unexpected successful token validation after supposedly expiring in cache, token spec: {}, now: {}\n",
		    tokenSpec.toStringRef(arena).toStringView(),
		    g_network->timer());
		ASSERT(false);
	}
	fmt::print("TEST OK\n");
	return Void();
}
