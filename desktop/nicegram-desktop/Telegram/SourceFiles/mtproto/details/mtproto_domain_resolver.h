/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"

#include <QtCore/QPointer>
#include <QtNetwork/QNetworkReply>
#include <optional>
#include "base/flat_set.h"
#include "rpl/lifetime.h"

namespace MTP::details {

[[nodiscard]] const std::vector<QString> &DnsDomains();
[[nodiscard]] QString GenerateDnsRandomPadding();
[[nodiscard]] QByteArray DnsUserAgent();

struct DnsEntry {
	QString data;
	crl::time TTL = 0;
};

[[nodiscard]] std::vector<DnsEntry> ParseDnsResponse(
	const QByteArray &bytes,
	std::optional<int> typeRestriction = std::nullopt);

struct ServiceWebRequest {
	ServiceWebRequest(not_null<QNetworkReply*> reply);
	ServiceWebRequest(ServiceWebRequest &&other);
	ServiceWebRequest &operator=(ServiceWebRequest &&other);
	~ServiceWebRequest();

	void destroy();

	QPointer<QNetworkReply> reply;
};

class DomainResolver : public QObject {
public:
	using Callback = Fn<void(
		const QString &domain,
		const QStringList &ips,
		crl::time expireAt)>;

	DomainResolver();

	// One resolver (one cache, one set of in-flight lookups) for the whole
	// process: with hundreds of accounts sharing a handful of proxy hosts,
	// per-account resolvers multiply into thousands of DNS-over-HTTPS
	// requests at startup, which get rate-limited and leave the proxy
	// hosts unresolved.
	[[nodiscard]] static DomainResolver &Shared();
	[[nodiscard]] rpl::lifetime subscribe(Callback callback);

	void resolve(const QString &domain);

private:
	enum class Type {
		Mozilla,
		Google,
	};
	struct Attempt {
		Type type;
		QString data;
		QString host;
	};
	struct AttemptKey {
		QString domain;
		bool ipv6 = false;

		inline bool operator<(const AttemptKey &other) const {
			return (domain < other.domain)
				|| (domain == other.domain && !ipv6 && other.ipv6);
		}
		inline bool operator==(const AttemptKey &other) const {
			return (domain == other.domain) && (ipv6 == other.ipv6);
		}
	};
	struct CacheEntry {
		QStringList ips;
		crl::time expireAt = 0;
	};
	struct Attempts {
		std::vector<Attempt> list;
		base::has_weak_ptr guard;
	};

	void resolve(const AttemptKey &key);
	void sendNextRequest(const AttemptKey &key);
	void performRequest(const AttemptKey &key, const Attempt &attempt);
	void checkExpireAndPushResult(const QString &domain);
	void requestFinished(
		const AttemptKey &key,
		not_null<QNetworkReply*> reply);
	QByteArray finalizeRequest(
		const AttemptKey &key,
		not_null<QNetworkReply*> reply);

	void notify(
		const QString &domain,
		const QStringList &ips,
		crl::time expireAt);
	void resolveViaDoh(const AttemptKey &key);

	std::map<int, Callback> _callbacks;
	int _nextCallbackId = 0;
	base::flat_set<AttemptKey> _systemLookups;

	QNetworkAccessManager _manager;
	std::map<AttemptKey, Attempts> _attempts;
	std::map<AttemptKey, std::vector<ServiceWebRequest>> _requests;
	std::map<AttemptKey, CacheEntry> _cache;
	crl::time _lastTimestamp = 0;

};

} // namespace MTP::details
