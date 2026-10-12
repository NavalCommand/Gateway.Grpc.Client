#include <NavalCommand/Gateway/Grpc/Client/Connection.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

using namespace NavalCommand;
using Gateway::Grpc::Client::Connection;
using Gateway::Grpc::Client::View;

namespace
{
	constexpr int MAXIMUM_PAUSE_POLLS = 50;
	constexpr int MAXIMUM_FIXTURE_POLLS = 100;
	constexpr int MAXIMUM_DETECTION_POLLS = 200;
	constexpr int MAXIMUM_RELOAD_POLLS = 900;
	constexpr std::chrono::milliseconds PUBLICATION_POLL_INTERVAL{100};
	constexpr std::chrono::milliseconds FIXTURE_POLL_INTERVAL{50};
	constexpr std::chrono::milliseconds PUBLICATION_SETTLE_INTERVAL{400};
	std::filesystem::path fixtureDirectory;
	int sequence{};
	void Require(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	std::shared_ptr<View> Read(Connection& connection, std::optional<Factions::Api::Id> faction = std::nullopt)
	{
		auto result = connection.Acquire(faction);
		Require(result.has_value(), "acquire failed");
		return *result;
	}

	void WaitPause(Connection& connection, bool expected)
	{
		for (int attempt = 0; attempt < MAXIMUM_PAUSE_POLLS; ++attempt)
		{
			auto value = Read(connection)->IsPaused();
			Require(value.has_value(), "pause read failed");
			if (*value == expected)
			{
				return;
			}
			std::this_thread::sleep_for(PUBLICATION_POLL_INTERVAL);
		}
		throw std::runtime_error("pause state did not converge");
	}

	void WaitCompression(Connection& connection, double expected)
	{
		for (int attempt = 0; attempt < MAXIMUM_PAUSE_POLLS; ++attempt)
		{
			auto value = Read(connection)->GetTimeCompression();
			Require(value.has_value(), "compression read failed");
			if (value->Value == expected)
			{
				return;
			}
			std::this_thread::sleep_for(PUBLICATION_POLL_INTERVAL);
		}
		throw std::runtime_error("selected compression did not converge");
	}

	double Native(const std::string& action, bool expectedPaused)
	{
		const auto identity = std::to_string(++sequence);
		const auto temporary = fixtureDirectory / "fixture-request.tmp";
		{
			std::ofstream file{temporary};
			file << identity << ' ' << action;
		}
		std::filesystem::remove(fixtureDirectory / "fixture-request.txt");
		std::filesystem::rename(temporary, fixtureDirectory / "fixture-request.txt");
		for (int attempt = 0; attempt < MAXIMUM_FIXTURE_POLLS; ++attempt)
		{
			std::ifstream response{fixtureDirectory / ("fixture-response-" + identity + ".txt")};
			if (response)
			{
				std::string state;
				double compression{}, time{};
				response >> state >> compression >> time;
				Require(bool(response), "invalid native fixture response");
				Require(state == (expectedPaused ? "paused" : "running"), "native pause state mismatch");
				std::cout << "native " << action << " " << state << " effectiveCompression=" << compression << " missionTime=" << time << '\n';
				return time;
			}
			std::this_thread::sleep_for(FIXTURE_POLL_INTERVAL);
		}
		throw std::runtime_error("native fixture timeout");
	}
}

// Run only against an explicitly authorized disposable mission. Each read is a real RPC.
int main(int argc, char** argv)
{
	std::string endpoint;
	bool reload{};
	bool requireContacts{};
	bool requireAirOperations{};
	for (int argument = 1; argument + 1 < argc; argument += 2)
	{
		if (std::string{argv[argument]} == "--endpoint")
		{
			endpoint = argv[argument + 1];
		}
		else if (std::string{argv[argument]} == "--fixture-directory")
		{
			fixtureDirectory = argv[argument + 1];
		}
		else if (std::string{argv[argument]} == "--expect-reload")
		{
			reload = std::string{argv[argument + 1]} == "true";
		}
		else if (std::string{argv[argument]} == "--require-contacts")
		{
			requireContacts = std::string{argv[argument + 1]} == "true";
		}
		else if (std::string{argv[argument]} == "--require-air-operations")
		{
			requireAirOperations = std::string{argv[argument + 1]} == "true";
		}
		else if (std::string{argv[argument]} != "--duration-ms")
		{
			return 2;
		}
	}
	if (endpoint.empty())
	{
		return 2;
	}
	try
	{
		Connection first{endpoint};
		Connection second{endpoint};
		auto view = Read(first);
		if (requireContacts)
		{
			bool observed = false;
			for (int attempt = 0; attempt < MAXIMUM_DETECTION_POLLS; ++attempt)
			{
				auto perspective = Read(first, Factions::Api::Id{"Taskforce1"});
				auto contacts = perspective->GetObservationIds();
				Require(contacts.has_value(), "observation read failed");
				if (!contacts->empty())
				{
					observed = true;
					break;
				}
				std::this_thread::sleep_for(PUBLICATION_POLL_INTERVAL);
			}
			Require(observed, "native detection did not produce contacts");
			view = Read(first);
		}
		auto ids = view->GetUnitInstanceIds();
		auto factions = view->GetFactionIds();
		Require(ids.has_value() && !ids->empty(), "no authoritative units");
		Require(factions.has_value() && factions->size() >= 2, "missing factions");
		std::size_t resourceFree = 0, resourceBacked = 0;
		std::size_t airOperations{};
		for (const auto& id : *ids)
		{
			auto unit = view->GetUnitInstance(id);
			Require(unit.has_value(), "unit read failed");
			Require(unit->Kinematics.Position.has_value(), "missing authoritative position");
			Require(std::isfinite(unit->Kinematics.Position->Latitude.Value) && std::isfinite(unit->Kinematics.Position->Longitude.Value), "nonfinite position");
			std::cout << "unit " << id.Value << " name=" << unit->Name << " faction=" << unit->Faction.Value << " equipment=" << unit->Equipment.size() << '\n';
			auto operations = view->GetUnitOperations(id);
			Require(operations.has_value(), "operations read failed");
			if (operations->AirOperationsState)
			{
				++airOperations;
				std::cout << "  native air operations available\n";
			}
			for (const auto& equipment : unit->Equipment)
			{
				std::cout << "  capability " << equipment.Type.Value;
				if (equipment.Inventory)
				{
					++resourceBacked;
					Require(!equipment.Inventory->ResourceId.Value.empty(), "empty real resource identity");
					std::cout << " resource=" << equipment.Inventory->ResourceId.Value << " quantity=";
					std::visit(
						[](auto quantity)
						{
							std::cout << quantity;
						},
						equipment.Inventory->Quantity);
				}
				else
				{
					++resourceFree;
					std::cout << " resource-free";
				}
				std::cout << '\n';
			}
		}
		Require(resourceFree > 0 && resourceBacked > 0, "missing resource-free or resource-backed equipment");
		Require(!requireAirOperations || airOperations > 0, "no native air operations");
		for (const auto& faction : *factions)
		{
			auto scoped = Read(first, faction);
			auto own = scoped->GetUnitInstanceIds();
			auto contacts = scoped->GetObservationIds();
			Require(own.has_value() && contacts.has_value(), "scope read failed");
			std::cout << "scope " << faction.Value << " own=" << own->size() << " contacts=" << contacts->size() << '\n';
			for (const auto& id : *ids)
			{
				auto truth = view->GetUnitInstance(id);
				auto selected = scoped->GetUnitInstance(id);
				Require(selected.has_value() == (truth->Faction == faction), "enemy truth crossed faction scope");
			}
			for (const auto& id : *contacts)
			{
				auto observation = scoped->GetObservationInstance(id);
				Require(observation.has_value() && observation->ObserverFaction == faction, "contact observer mismatch");
				Require(!observation->Kinematics.Speed.IsKnown() && !observation->Kinematics.Heading.IsKnown(), "unverified contact kinematics exposed");
				std::cout << "  observer contact " << id.Value << " name=" << observation->ObservationName << " unknown speed and heading retained\n";
			}
		}
		WaitPause(first, false);
		auto originalCompression = view->GetTimeCompression();
		Require(originalCompression.has_value(), "compression read failed");
		Require(Read(first)->RequestPause(true).has_value(), "first pause rejected");
		WaitPause(second, true);
		Require(Read(second)->RequestPause(true).has_value(), "second pause rejected");
		Require(Read(first)->SetTimeCompression({2}).has_value(), "paused compression selection rejected");
		std::this_thread::sleep_for(PUBLICATION_SETTLE_INTERVAL);
		WaitCompression(first, 2);
		WaitPause(second, true);
		first.Stop();
		std::this_thread::sleep_for(PUBLICATION_SETTLE_INTERVAL);
		WaitPause(second, true);
		Require(Read(second)->RequestPause(false).has_value(), "remaining owner release rejected");
		WaitPause(second, false);
		Connection reconnected{endpoint};
		WaitPause(reconnected, false);
		Require(Read(reconnected)->RequestPause(true).has_value(), "fresh owner pause rejected");
		WaitPause(second, true);
		reconnected.Stop();
		WaitPause(second, false);
		Require(Read(second)->SetTimeCompression(*originalCompression).has_value(), "compression restore rejected");
		if (!fixtureDirectory.empty())
		{
			Native("pause", true);
			WaitPause(second, true);
			Require(Read(second)->RequestPause(true).has_value(), "client claim with native owner rejected");
			Native("unpause", true);
			WaitPause(second, true);
			const double frozen = Native("read", true);
			std::this_thread::sleep_for(std::chrono::milliseconds{600});
			Require(Native("read", true) == frozen, "paused simulation time advanced");
			Native("compression2", true);
			std::this_thread::sleep_for(PUBLICATION_SETTLE_INTERVAL);
			WaitCompression(second, 2);
			Require(Read(second)->RequestPause(false).has_value(), "client release after native compression rejected");
			WaitPause(second, false);
			Native("pause", true);
			Require(Read(second)->RequestPause(true).has_value(), "last client pause rejected");
			second.Stop();
			std::this_thread::sleep_for(std::chrono::milliseconds{500});
			Native("read", true);
			Connection fresh{endpoint};
			WaitPause(fresh, true);
			Require(Read(fresh)->RequestPause(false).has_value(), "fresh no-claim release rejected");
			WaitPause(fresh, true);
			Native("unpause", false);
			WaitPause(fresh, false);
			Require(Read(fresh)->SetTimeCompression(*originalCompression).has_value(), "native test compression restore rejected");
			std::cout << "PASS native player ownership, native compression selector, frozen time, disconnect preserves native owner\n";
		}
		if (reload)
		{
			Connection monitor{endpoint};
			const auto old = Read(monitor);
			bool changed = false;
			for (int attempt = 0; attempt < MAXIMUM_RELOAD_POLLS; ++attempt)
			{
				auto current = monitor.Acquire();
				if (current && (*current)->Session() != old->Session())
				{
					Require(!old->GetUnitInstanceIds().has_value(), "old mission view remained valid");
					Require((*current)->GetUnitInstanceIds().has_value(), "replacement mission unavailable");
					WaitPause(monitor, false);
					changed = true;
					break;
				}
				std::this_thread::sleep_for(PUBLICATION_POLL_INTERVAL);
			}
			Require(changed, "mission reload did not produce a new session");
			std::cout << "PASS real mission reload invalidates old view and restores fresh ownership\n";
		}
		std::cout << "PASS live data, faction isolation, shared client pause ownership, paused compression, disconnect and fresh reconnect\n";
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "FAIL " << exception.what() << '\n';
		return 1;
	}
}
