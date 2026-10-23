/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/mtproto_proxy_data.h"

#include <rpl/event_stream.h>

namespace MTP {

struct ProxyList {
	QString name;
	std::vector<ProxyData> entries;
};

// Named, on-disk pools of spare SOCKS5 proxies (e.g. one list per
// region/source: "India", "Indonesia", ...), entirely separate from the
// app's own global proxy list/settings. Meant for bulk-loading a batch of
// proxies once per list (paste as ip:port:login:pass, one per line) and
// then handing one out automatically - by list name - when adding
// accounts. Taking an entry removes it from its list, so nothing gets
// assigned twice.
class ProxyPool final {
public:
	ProxyPool();

	[[nodiscard]] const std::vector<ProxyList> &lists() const;
	// Fires after every change that was written to disk (lists created,
	// renamed, removed, entries added or taken) - screens showing the
	// lists refresh from it instead of being reopened.
	[[nodiscard]] rpl::producer<> changes() const;

	// Returns the new list's index.
	int addList(const QString &name);
	void renameList(int listIndex, const QString &name);
	void removeList(int listIndex);

	// Parses "host:port[:login[:password]]" lines (blank/unparsable
	// lines skipped). Returns how many were actually added.
	int addLines(int listIndex, const QString &text);
	void removeEntry(int listIndex, int entryIndex);

	// Removes and returns the first entry of the list, or nullopt if the
	// list is empty or listIndex is out of range.
	[[nodiscard]] std::optional<ProxyData> take(int listIndex);

	// Reads the first entry of the list without removing it, or nullopt.
	[[nodiscard]] std::optional<ProxyData> peek(int listIndex) const;

	// Removes the first entry across all lists that's structurally equal
	// to "proxy" (used to consume a previously-peek()'d entry only once
	// the thing it was picked for is confirmed to have actually
	// succeeded, instead of removing it up front and losing it on a
	// failed add attempt). Returns whether an entry was found and removed.
	bool removeMatching(const ProxyData &proxy);

	[[nodiscard]] QByteArray toJson() const;
	int mergeJson(const QByteArray &data);

private:
	void load();
	void save() const;

	std::vector<ProxyList> _lists;
	mutable rpl::event_stream<> _changes;

};

[[nodiscard]] ProxyPool &GlobalProxyPool();

// A SOCKS5 address that can never actually connect (unassigned loopback
// port) - used as a stand-in when an account needs *some* proxy applied
// (so it never falls back to a direct/unproxied connection) but none is
// actually available yet, e.g. importing more accounts from a tdata folder
// than a chosen proxy list has entries for. Shows up as permanently
// disconnected until the user sets a real proxy for that account.
[[nodiscard]] ProxyData UnreachableProxyPlaceholder();

// Whether `proxy` is a real, usable proxy of an account: set, with a host,
// and not the stand-in above. An account that holds a session never
// connects without one (see Main::Account::startMtp).
[[nodiscard]] inline bool HasRealProxy(const ProxyData &proxy) {
	const auto placeholder = UnreachableProxyPlaceholder();
	return (proxy.type != ProxyData::Type::None)
		&& !proxy.host.isEmpty()
		&& !(proxy.host == placeholder.host
			&& proxy.port == placeholder.port);
}

} // namespace MTP
