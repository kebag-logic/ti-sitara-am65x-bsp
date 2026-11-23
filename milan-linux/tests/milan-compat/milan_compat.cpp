// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// milan_compat.cpp - a controller on L-Acoustics' la_avdecc (4.3.x): discover
// one entity on an interface, enumerate it as la_avdecc's controller does
// (every descriptor read, GET_MILAN_INFO, the dynamic state, counters,
// unsolicited notifications), with the compatibility checks it runs on every
// answer, and print every compatibility event and every warning or error the
// library logs. The verdict at the end is the library's own: the entity must
// keep IEEE17221 and Milan, gain none of IEEE17221Warning, MilanWarning or
// Misbehaving, keep the Milan version GET_MILAN_INFO gave, and the library
// must have logged no compatibility event, warning or error.
//
// usage: milan-compat IFACE [--entity EID] [--wait S] [--settle S]
//   --entity  the entity to judge (hex); without it, the first one online
//   --wait    how long to wait for it to come online (20 s)
//   --settle  how long to keep listening once it is (10 s): unsolicited
//             notifications, counters, anything the library re-checks
// Exit 0 for a Milan-compatible entity with nothing logged, 1 otherwise.

#include <la/avdecc/avdecc.hpp>
#include <la/avdecc/controller/avdeccController.hpp>
#include <la/avdecc/logger.hpp>
#include <la/avdecc/utils.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace avdecc = la::avdecc;
namespace controller = la::avdecc::controller;
using Flag = controller::ControlledEntity::CompatibilityFlag;

static std::mutex out_lock;

static void say(std::string const& line)
{
	std::lock_guard<std::mutex> g(out_lock);
	std::fprintf(stdout, "%s\n", line.c_str());
	std::fflush(stdout);
}

static std::string hex(std::uint64_t v)
{
	char b[24];
	std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
	return b;
}

static std::string flags_text(controller::ControlledEntity::CompatibilityFlags const f)
{
	std::string s;
	struct
	{
		Flag flag;
		char const* name;
	} const names[] = {{Flag::IEEE17221, "IEEE17221"}, {Flag::Milan, "Milan"}, {Flag::IEEE17221Warning, "IEEE17221Warning"},
			   {Flag::MilanWarning, "MilanWarning"}, {Flag::Misbehaving, "Misbehaving"}};
	for (auto const& n : names) {
		if (f.test(n.flag)) {
			s += (s.empty() ? "" : "|") + std::string(n.name);
		}
	}
	return s.empty() ? "None" : s;
}

static char const* level_text(avdecc::logger::Level const l)
{
	switch (l) {
	case avdecc::logger::Level::Trace: return "trace";
	case avdecc::logger::Level::Debug: return "debug";
	case avdecc::logger::Level::Info: return "info";
	case avdecc::logger::Level::Warn: return "WARNING";
	case avdecc::logger::Level::Error: return "ERROR";
	case avdecc::logger::Level::Compat: return "COMPAT";
	default: return "?";
	}
}

// the libraries' compile options, the leniencies among them (IGNORE_..., CONTINUE_...) included
static std::string options_text()
{
	std::string s;
	auto const a = avdecc::getCompileOptions();
	for (auto const& info : avdecc::getCompileOptionsInfo()) {
		if (a.test(info.option)) {
			s += (s.empty() ? "" : " ") + info.shortName;
		}
	}
	auto const c = controller::getCompileOptions();
	for (auto const& info : controller::getCompileOptionsInfo()) {
		if (c.test(info.option)) {
			s += (s.empty() ? "" : " ") + info.shortName;
		}
	}
	return s;
}

class Judge : public controller::Controller::DefaultedObserver, public avdecc::logger::Logger::Observer
{
public:
	explicit Judge(std::uint64_t target) : _target(target) {}

	// every warning, error and compatibility item the library logs
	void onLogItem(avdecc::logger::Level const level, avdecc::logger::LogItem const* const item) noexcept override
	{
		bool counted = level == avdecc::logger::Level::Warn || level == avdecc::logger::Level::Error ||
			       level == avdecc::logger::Level::Compat;
		if (counted) {
			_logged++;
		}
		if (counted || _verbose) {
			say(std::string("log ") + level_text(level) + ": " + item->getMessage());
		}
	}

	void onEntityOnline(controller::Controller const* const, controller::ControlledEntity const* const e) noexcept override
	{
		auto const id = e->getEntity().getEntityID().getValue();
		say("online: entity " + hex(id) + " model " + hex(e->getEntity().getEntityModelID().getValue()) +
		    ", compatibility " + flags_text(e->getCompatibilityFlags()) + ", Milan " +
		    e->getMilanCompatibilityVersion().to_string() + ", " +
		    (e->isSubscribedToUnsolicitedNotifications() ? "subscribed" : "NOT subscribed") +
		    " to unsolicited notifications, GET_DYNAMIC_INFO " + (e->isPackedDynamicInfoSupported() ? "used" : "not used"));
		for (auto const& ev : e->getCompatibilityChangedEvents()) {
			event_line(ev);
		}
		if (_target == 0 || id == _target) {
			std::lock_guard<std::mutex> g(_m);
			if (_online == 0) {
				_online = id;
				_milan_at_online = e->getMilanCompatibilityVersion().to_string();
			}
			_cv.notify_all();
		}
	}

	void onEntityOffline(controller::Controller const* const, controller::ControlledEntity const* const e) noexcept override
	{
		say("offline: entity " + hex(e->getEntity().getEntityID().getValue()));
		if (e->getEntity().getEntityID().getValue() == _online) {
			_went_offline = true;
		}
	}

	void onEntityQueryError(controller::Controller const* const, controller::ControlledEntity const* const e,
				controller::Controller::QueryCommandError const error) noexcept override
	{
		_errors++;
		say("query error: entity " + hex(e->getEntity().getEntityID().getValue()) + " error " +
		    std::to_string(avdecc::utils::to_integral(error)));
	}

	void onCompatibilityChanged(controller::Controller const* const, controller::ControlledEntity const* const e,
				    controller::ControlledEntity::CompatibilityFlags const flags,
				    avdecc::entity::model::MilanVersion const& version) noexcept override
	{
		say("compatibility changed: entity " + hex(e->getEntity().getEntityID().getValue()) + " now " +
		    flags_text(flags) + ", Milan " + version.to_string());
		auto const& events = e->getCompatibilityChangedEvents();
		if (!events.empty()) {
			event_line(events.back());
		}
	}

	void onUnsolicitedRegistrationChanged(controller::Controller const* const, controller::ControlledEntity const* const e,
					      bool const subscribed, bool const by_entity) noexcept override
	{
		say("unsolicited notifications: entity " + hex(e->getEntity().getEntityID().getValue()) +
		    (subscribed ? " subscribed" : " not subscribed") + (by_entity ? " (by the entity)" : ""));
	}

	void onStreamInputCountersChanged(controller::Controller const* const, controller::ControlledEntity const* const,
					  avdecc::entity::model::StreamIndex const index,
					  avdecc::entity::model::StreamInputCounters const& counters) noexcept override
	{
		std::string s;
		for (auto const& [flag, value] : counters) {
			s += " " + std::to_string(avdecc::utils::to_integral(flag)) + "=" + std::to_string(value);
		}
		say("counters STREAM_INPUT " + std::to_string(index) + " (valid bit value=count):" + s);
	}

	void onStreamOutputCountersChanged(controller::Controller const* const, controller::ControlledEntity const* const,
					   avdecc::entity::model::StreamIndex const index,
					   avdecc::entity::model::StreamOutputCounters const& counters) noexcept override
	{
		std::string s;
		for (auto const& [flag, value] : counters.getCounters<avdecc::entity::StreamOutputCounterValidFlagsMilan12>()) {
			s += " " + std::to_string(avdecc::utils::to_integral(flag)) + "=" + std::to_string(value);
		}
		say("counters STREAM_OUTPUT " + std::to_string(index) + " (Milan 1.2 layout, valid bit value=count):" + s);
	}

	void onClockDomainCountersChanged(controller::Controller const* const, controller::ControlledEntity const* const,
					  avdecc::entity::model::ClockDomainIndex const index,
					  avdecc::entity::model::ClockDomainCounters const& counters) noexcept override
	{
		std::string s;
		for (auto const& [flag, value] : counters) {
			s += " " + std::to_string(avdecc::utils::to_integral(flag)) + "=" + std::to_string(value);
		}
		say("counters CLOCK_DOMAIN " + std::to_string(index) + ":" + s);
	}

	void onAvbInterfaceCountersChanged(controller::Controller const* const, controller::ControlledEntity const* const,
					   avdecc::entity::model::AvbInterfaceIndex const index,
					   avdecc::entity::model::AvbInterfaceCounters const& counters) noexcept override
	{
		std::string s;
		for (auto const& [flag, value] : counters) {
			s += " " + std::to_string(avdecc::utils::to_integral(flag)) + "=" + std::to_string(value);
		}
		say("counters AVB_INTERFACE " + std::to_string(index) + ":" + s);
	}

	bool wait_online(std::chrono::seconds const s)
	{
		std::unique_lock<std::mutex> g(_m);
		return _cv.wait_for(g, s, [this] { return _online != 0; });
	}

	std::uint64_t online() const
	{
		return _online;
	}
	std::string milan_at_online() const
	{
		return _milan_at_online;
	}
	unsigned logged() const
	{
		return _logged;
	}
	unsigned errors() const
	{
		return _errors;
	}
	bool went_offline() const
	{
		return _went_offline;
	}
	void verbose(bool v)
	{
		_verbose = v;
	}

private:
	static void event_line(controller::ControlledEntity::CompatibilityChangedEvent const& ev)
	{
		say("compatibility event: [" + ev.specClause + "] " + ev.message + " (" + flags_text(ev.previousFlags) + " -> " +
		    flags_text(ev.newFlags) + ", Milan " + ev.previousMilanVersion.to_string() + " -> " +
		    ev.newMilanVersion.to_string() + ")");
	}

	std::uint64_t const _target;
	std::mutex _m;
	std::condition_variable _cv;
	std::uint64_t _online{0};
	std::string _milan_at_online;
	std::atomic<unsigned> _logged{0};
	std::atomic<unsigned> _errors{0};
	std::atomic<bool> _went_offline{false};
	std::atomic<bool> _verbose{false};
};

template<typename Map>
static std::string counters_text(std::optional<Map> const& m)
{
	if (!m) {
		return " none read";
	}
	std::string s;
	for (auto const& [flag, value] : *m) {
		s += " " + std::to_string(avdecc::utils::to_integral(flag)) + "=" + std::to_string(value);
	}
	return s;
}

// What the controller holds of the entity at the end: each stream's connection and counters, the
// clock domain's and the AVB interface's counters (valid bit value=count).
static void dump(controller::ControlledEntity const& e)
{
	try {
		auto const& cfg = e.getCurrentConfigurationNode();
		for (auto const& [index, node] : cfg.streamInputs) {
			auto const& d = node.dynamicModel;
			say("model STREAM_INPUT " + std::to_string(index) + " '" + std::string(d.objectName) + "' state " +
			    std::to_string(avdecc::utils::to_integral(d.connectionInfo.state)) + " talker " +
			    hex(d.connectionInfo.talkerStream.entityID.getValue()) + "/" +
			    std::to_string(d.connectionInfo.talkerStream.streamIndex) + ", format " +
			    hex(d.streamFormat.getValue()) + ", counters" + counters_text(d.counters));
		}
		for (auto const& [index, node] : cfg.streamOutputs) {
			auto const& d = node.dynamicModel;
			std::string s = " none read";
			if (d.counters) {
				s.clear();
				for (auto const& [flag, value] :
				     d.counters->getCounters<avdecc::entity::StreamOutputCounterValidFlagsMilan12>()) {
					s += " " + std::to_string(avdecc::utils::to_integral(flag)) + "=" + std::to_string(value);
				}
			}
			say("model STREAM_OUTPUT " + std::to_string(index) + " format " + hex(d.streamFormat.getValue()) +
			    ", counters (Milan 1.2 layout)" + s);
		}
		for (auto const& [index, node] : cfg.clockDomains) {
			say("model CLOCK_DOMAIN " + std::to_string(index) + " source " +
			    std::to_string(node.dynamicModel.clockSourceIndex) + ", counters" +
			    counters_text(node.dynamicModel.counters));
		}
		for (auto const& [index, node] : cfg.avbInterfaces) {
			auto const& d = node.dynamicModel;
			say("model AVB_INTERFACE " + std::to_string(index) + " clock identity " + hex(d.clockIdentity.getValue()) +
			    ", gPTP grandmaster " + hex(d.gptpGrandmasterID.getValue()) + ", AS path " +
			    (d.asPath ? std::to_string(d.asPath->sequence.size()) + " entries" : std::string("none")) +
			    ", counters" + counters_text(d.counters));
		}
	} catch (std::exception const& ex) {
		say(std::string("model: ") + ex.what());
	}
}

int main(int argc, char** argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: milan-compat IFACE [--entity EID] [--wait S] [--settle S] [--verbose]\n");
		return 2;
	}
	std::string const ifname = argv[1];
	std::uint64_t target = 0;
	long wait_s = 20, settle_s = 10;
	bool verbose = false;
	for (int i = 2; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--entity") && i + 1 < argc) {
			target = std::strtoull(argv[++i], nullptr, 16);
		} else if (!std::strcmp(argv[i], "--wait") && i + 1 < argc) {
			wait_s = std::strtol(argv[++i], nullptr, 0);
		} else if (!std::strcmp(argv[i], "--settle") && i + 1 < argc) {
			settle_s = std::strtol(argv[++i], nullptr, 0);
		} else if (!std::strcmp(argv[i], "--verbose")) {
			verbose = true;
		} else {
			std::fprintf(stderr, "milan-compat: unknown argument %s\n", argv[i]);
			return 2;
		}
	}
	say(std::string("la_avdecc ") + avdecc::getVersion() + ", controller library " + controller::getVersion() +
	    ", compiled with: " + options_text() + "; on " + ifname);

	Judge judge(target);
	judge.verbose(verbose);
	auto& logger = avdecc::logger::Logger::getInstance();
	logger.setLevel(avdecc::logger::Level::Info);
	logger.registerObserver(&judge);
	int rc = 1;
	try {
		auto c = controller::Controller::create(avdecc::protocol::ProtocolInterface::Type::PCap, ifname, 0x0003,
						       avdecc::UniqueIdentifier::getNullUniqueIdentifier(), "en-US", nullptr,
						       std::nullopt, nullptr);
		// read every descriptor, as a first enumeration does, not only what GET_DYNAMIC_INFO packs
		c->enableFullStaticEntityModelEnumeration();
		c->registerObserver(&judge);
		if (!judge.wait_online(std::chrono::seconds(wait_s))) {
			say("verdict: FAIL - no entity" + (target ? " " + hex(target) : std::string()) + " online in " +
			    std::to_string(wait_s) + " s");
		} else {
			say("settling " + std::to_string(settle_s) + " s");
			std::this_thread::sleep_for(std::chrono::seconds(settle_s));
			auto e = c->getControlledEntityGuard(avdecc::UniqueIdentifier{judge.online()});
			if (!e) {
				say("verdict: FAIL - the entity went away");
			} else {
				auto const f = e->getCompatibilityFlags();
				auto const& events = e->getCompatibilityChangedEvents();
				bool clean = f.test(Flag::IEEE17221) && f.test(Flag::Milan) && !f.test(Flag::IEEE17221Warning) &&
					     !f.test(Flag::MilanWarning) && !f.test(Flag::Misbehaving);
				bool same_version = e->getMilanCompatibilityVersion().to_string() == judge.milan_at_online();
				say("final: entity " + hex(judge.online()) + ", compatibility " + flags_text(f) + ", Milan " +
				    e->getMilanCompatibilityVersion().to_string() + ", " + std::to_string(events.size()) +
				    " compatibility events, " + std::to_string(judge.logged()) + " warnings/errors logged, " +
				    std::to_string(judge.errors()) + " query errors, fatal enumeration error " +
				    (e->gotFatalEnumerationError() ? "yes" : "no"));
				for (auto const& ev : events) {
					say("final event: [" + ev.specClause + "] " + ev.message);
				}
				dump(*e);
				if (clean && same_version && events.empty() && judge.logged() == 0 && judge.errors() == 0 &&
				    !e->gotFatalEnumerationError() && !judge.went_offline()) {
					say("verdict: PASS - Milan " + e->getMilanCompatibilityVersion().to_string() +
					    " compatible, zero compatibility events, warnings or errors");
					rc = 0;
				} else {
					say("verdict: FAIL");
				}
			}
		}
		c->unregisterObserver(&judge);
	} catch (std::exception const& ex) {
		say(std::string("verdict: FAIL - ") + ex.what());
	}
	logger.unregisterObserver(&judge);
	return rc;
}
