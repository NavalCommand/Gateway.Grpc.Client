#pragma once

// Windows defines this macro with the same name as the domain time method.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <NavalCommand/Classification/Api/Alignment.h>
#include <NavalCommand/Environment/Api/ITimeProvider.h>
#include <NavalCommand/Equipment/Api/IProvider.h>
#include <NavalCommand/Factions/Api/IProvider.h>
#include <NavalCommand/Observations/Api/IInstanceProvider.h>
#include <NavalCommand/Operations/Api/IProvider.h>
#include <NavalCommand/Resources/Api/IProvider.h>
#include <NavalCommand/Simulation/Api/IPauseController.h>
#include <NavalCommand/Simulation/Api/IPauseProvider.h>
#include <NavalCommand/Simulation/Api/ITimeCompressionController.h>
#include <NavalCommand/Simulation/Api/ITimeCompressionProvider.h>
#include <NavalCommand/Units/Api/IInstanceProvider.h>
#include <NavalCommand/Units/Api/ITypeProvider.h>
#include <chrono>
#include <memory>
#include <optional>

namespace NavalCommand::Gateway::Grpc::Client
{
	class Transport;

	// Retains a remote logical revision. Every read remains synchronous and
	// performs an RPC against this revision; application replication is external.
	class View final : public Units::Api::IInstanceProvider,
					   public Observations::Api::IInstanceProvider,
					   public Operations::Api::IProvider,
					   public Factions::Api::IProvider,
					   public Units::Api::ITypeProvider,
					   public Equipment::Api::IProvider,
					   public Resources::Api::IProvider,
					   public Environment::Api::ITimeProvider,
					   public Simulation::Api::IPauseProvider,
					   public Simulation::Api::IPauseController,
					   public Simulation::Api::ITimeCompressionProvider,
					   public Simulation::Api::ITimeCompressionController
	{
		friend class Connection;
		std::shared_ptr<Transport> m_Transport;
		std::string m_Client;
		std::string m_Token;
		std::string m_Session;
		std::uint64_t m_Revision{};
		View(std::shared_ptr<Transport> transport, std::string client, std::string token, std::string session, std::uint64_t revision);

	public:
		const std::string& Session() const;
		std::uint64_t Revision() const;
		Error::Api::Result<std::vector<Units::Api::Id>> GetUnitInstanceIds() const override;
		Error::Api::Result<Units::Api::Instance> GetUnitInstance(const Units::Api::Id& id) const override;
		Error::Api::Result<std::vector<Observations::Api::Id>> GetObservationIds() const override;
		Error::Api::Result<Observations::Api::Instance> GetObservationInstance(const Observations::Api::Id& id) const override;
		Error::Api::Result<Operations::Api::UnitState> GetUnitOperations(const Units::Api::Id& id) const override;
		Error::Api::Result<std::vector<Factions::Api::Id>> GetFactionIds() override;
		Error::Api::DomainResult<Classification::Api::Alignment, Factions::Api::Error> GetFactionAlignment(const Factions::Api::Id& id, const Factions::Api::Id& second) override;
		Error::Api::Result<std::vector<Units::Api::TypeId>> GetUnitTypeIds() override;
		Error::Api::Result<Units::Api::Type> GetUnitType(const Units::Api::TypeId& id) override;
		Error::Api::Result<std::vector<Equipment::Api::TypeId>> GetEquipmentTypes() override;
		Error::Api::DomainResult<Equipment::Api::Type, Equipment::Api::Error> GetEquipmentType(const Equipment::Api::TypeId& id) const override;
		Error::Api::Result<std::vector<Resources::Api::Id>> GetResourceIds() const override;
		Error::Api::Result<std::vector<Resources::Api::Definition>> GetResourceDefinition(const Resources::Api::Id& id) const override;
		Error::Api::Result<Physics::Api::Seconds> GetCurrentTime() override;
		Error::Api::Result<bool> IsPaused() const override;
		Error::Api::Result<bool> CanRequestPause() const override;
		Error::Api::Result<bool> CanRequestUnpause() const override;
		Error::Api::Result<Simulation::Api::TimeCompressionFactor> GetTimeCompression() override;
		Error::Api::DomainResult<void, Simulation::Api::Error> RequestPause(bool paused) override;
		Error::Api::DomainResult<void, Simulation::Api::Error> SetTimeCompression(Simulation::Api::TimeCompressionFactor factor) override;
	};

	class Connection final
	{
		std::shared_ptr<Transport> m_Transport;

	public:
		explicit Connection(std::string endpoint, std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});
		~Connection();
		Connection(const Connection&) = delete;
		Connection& operator=(const Connection&) = delete;
		// nullopt selects explicit privileged omniscience; no sentinel faction.
		Error::Api::Result<std::shared_ptr<View>> Acquire(std::optional<Factions::Api::Id> faction = std::nullopt);
		bool Connected() const;
		void Stop();
	};
}
