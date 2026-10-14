#include <NavalCommand/Classification/Api/Alignment.h>
#include <NavalCommand/Error/Api/MakeError.h>
#include <NavalCommand/Gateway/Api/Codec.h>
#include <NavalCommand/Gateway/Grpc/Client/Connection.h>
#include <atomic>
#include <condition_variable>
#include <grpcpp/grpcpp.h>
#include <mutex>
#include <navalcommand/gateway/v2/service.grpc.pb.h>
#include <thread>

// Windows defines a function-like macro with the domain clock method's name.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

namespace NavalCommand::Gateway::Grpc::Client
{
	namespace Wire = Api::Wire;
	namespace
	{
		Error::Api::Error TransportError(const grpc::Status& status)
		{
			using Condition = Error::Api::ErrorCondition;
			switch (status.error_code())
			{
			case grpc::StatusCode::PERMISSION_DENIED:
			case grpc::StatusCode::UNAUTHENTICATED:
				return Error::Api::MakeError(Condition::PermissionDenied);
			case grpc::StatusCode::RESOURCE_EXHAUSTED:
				return Error::Api::MakeError(Condition::ResourceExhausted);
			case grpc::StatusCode::INVALID_ARGUMENT:
				return Error::Api::MakeError(Condition::ContractError);
			case grpc::StatusCode::FAILED_PRECONDITION:
				return Error::Api::MakeError(Condition::NotReady);
			default:
				return Error::Api::MakeError(Condition::CommunicationError);
			}
		}

		Wire::Query Query(Wire::Query::Method method, const std::string& id = {}, const std::string& second = {})
		{
			Wire::Query query;
			query.set_method(method);
			query.set_id(id);
			query.set_second_id(second);
			return query;
		}

		template <class Result> Result DecodeFailure(const Wire::Failure& failure)
		{
			using ErrorType = typename Result::error_type;
			try
			{
				if (failure.has_infrastructure())
				{
					return std::unexpected(ErrorType{Api::Decode(failure.infrastructure())});
				}
				if constexpr (std::is_same_v<ErrorType, Error::Api::CombinedError<Simulation::Api::Error>>)
				{
					if (failure.has_simulation() && failure.simulation() >= 0 && failure.simulation() <= 1)
					{
						return std::unexpected(ErrorType{static_cast<Simulation::Api::Error>(failure.simulation())});
					}
				}
				if constexpr (std::is_same_v<ErrorType, Error::Api::CombinedError<Factions::Api::Error>>)
				{
					if (failure.has_faction() && failure.faction() == 0)
					{
						return std::unexpected(ErrorType{Factions::Api::Error::InvalidId});
					}
				}
				if constexpr (std::is_same_v<ErrorType, Error::Api::CombinedError<Equipment::Api::Error>>)
				{
					if (failure.has_equipment() && failure.equipment() == 0)
					{
						return std::unexpected(ErrorType{Equipment::Api::Error::InvalidId});
					}
				}
			}
			catch (const std::invalid_argument&)
			{
			}
			return std::unexpected(ErrorType{Error::Api::MakeError(Error::Api::ErrorCondition::ContractError)});
		}
	}

	class Transport
	{
		std::unique_ptr<Wire::Gateway::Stub> m_Stub;
		std::chrono::milliseconds m_Timeout;
		mutable std::mutex m_Mutex;
		std::condition_variable m_Changed;
		std::string m_Client;
		grpc::ClientContext* m_StreamContext{};
		std::jthread m_Worker;

	public:
		Transport(const std::string& endpoint, std::chrono::milliseconds timeout) : m_Timeout{timeout}
		{
			if (!endpoint.starts_with("127.0.0.1:") || timeout.count() <= 0)
			{
				throw std::invalid_argument("loopback_endpoint_and_positive_timeout_required");
			}
			grpc::ChannelArguments arguments;
			arguments.SetMaxReceiveMessageSize(16 * 1024 * 1024);
			m_Stub = Wire::Gateway::NewStub(grpc::CreateCustomChannel(endpoint, grpc::InsecureChannelCredentials(), arguments));
			m_Worker = std::jthread(
				[this](std::stop_token stop)
				{
					while (!stop.stop_requested())
					{
						grpc::ClientContext context;
						{
							std::lock_guard lock{m_Mutex};
							if (stop.stop_requested())
							{
								break;
							}
							m_StreamContext = &context;
						}
						auto stream = m_Stub->Connect(&context, Wire::ConnectionRequest{});
						Wire::ConnectionState state;
						while (stream->Read(&state))
						{
							std::lock_guard lock{m_Mutex};
							m_Client = state.client();
							m_Changed.notify_all();
						}
						stream->Finish();
						{
							std::unique_lock lock{m_Mutex};
							m_StreamContext = nullptr;
							m_Client.clear();
							m_Changed.notify_all();
							m_Changed.wait_for(lock,
											   std::chrono::milliseconds{250},
											   [&]
											   {
												   return stop.stop_requested();
											   });
						}
					}
				});
		}

		~Transport()
		{
			Stop();
		}

		void Stop()
		{
			m_Worker.request_stop();
			{
				std::lock_guard lock{m_Mutex};
				if (m_StreamContext)
				{
					m_StreamContext->TryCancel();
				}
				m_Changed.notify_all();
			}
			if (m_Worker.joinable())
			{
				m_Worker.join();
			}
		}

		bool Connected() const
		{
			std::lock_guard lock{m_Mutex};
			return !m_Client.empty();
		}

		Error::Api::Result<std::pair<std::string, Wire::Revision>> Acquire(const std::optional<Factions::Api::Id>& faction)
		{
			Wire::AcquireRequest request;
			{
				std::unique_lock lock{m_Mutex};
				if (!m_Changed.wait_for(lock,
										m_Timeout,
										[&]
										{
											return !m_Client.empty() || m_Worker.get_stop_token().stop_requested();
										}) ||
					m_Client.empty())
				{
					return std::unexpected(Error::Api::MakeError(Error::Api::ErrorCondition::CommunicationError));
				}
				request.set_client(m_Client);
			}
			if (faction)
			{
				request.mutable_scope()->set_faction(faction->Value);
			}
			else
			{
				request.mutable_scope()->mutable_omniscient();
			}
			grpc::ClientContext context;
			context.set_deadline(std::chrono::system_clock::now() + m_Timeout);
			Wire::Revision revision;
			auto status = m_Stub->Acquire(&context, request, &revision);
			if (!status.ok())
			{
				return std::unexpected(TransportError(status));
			}
			return std::pair{request.client(), revision};
		}

		Error::Api::Result<Wire::Result> Read(const std::string& client, const std::string& token, const Wire::Query& query)
		{
			Wire::ReadRequest request;
			request.set_client(client);
			request.set_token(token);
			*request.mutable_query() = query;
			grpc::ClientContext context;
			context.set_deadline(std::chrono::system_clock::now() + m_Timeout);
			Wire::Result result;
			auto status = m_Stub->Read(&context, request, &result);
			if (!status.ok())
			{
				return std::unexpected(TransportError(status));
			}
			return result;
		}

		Error::Api::DomainResult<void, Simulation::Api::Error> Apply(const std::string& client, Wire::Control control)
		{
			static std::atomic<std::uint64_t> nextRequest{1};
			control.set_request(std::to_string(nextRequest.fetch_add(1)));
			Wire::ApplyRequest request;
			request.set_client(client);
			*request.mutable_control() = control;
			grpc::ClientContext context;
			context.set_deadline(std::chrono::system_clock::now() + m_Timeout);
			Wire::Result result;
			auto status = m_Stub->Apply(&context, request, &result);
			using Result = Error::Api::DomainResult<void, Simulation::Api::Error>;
			if (!status.ok())
			{
				return std::unexpected(Result::error_type{TransportError(status)});
			}
			if (result.has_failure())
			{
				return DecodeFailure<Result>(result.failure());
			}
			if (!result.has_success())
			{
				return std::unexpected(Result::error_type{Error::Api::MakeError(Error::Api::ErrorCondition::ContractError)});
			}
			return {};
		}
	};

	Connection::Connection(std::string endpoint, std::chrono::milliseconds timeout) : m_Transport{std::make_shared<Transport>(endpoint, timeout)}
	{
	}

	Connection::~Connection()
	{
		Stop();
	}
	void Connection::Stop()
	{
		m_Transport->Stop();
	}
	bool Connection::Connected() const
	{
		return m_Transport->Connected();
	}

	Error::Api::Result<std::shared_ptr<View>> Connection::Acquire(std::optional<Factions::Api::Id> faction)
	{
		auto acquired = m_Transport->Acquire(faction);
		if (!acquired)
		{
			return std::unexpected(acquired.error());
		}
		const auto& [client, revision] = *acquired;
		return std::shared_ptr<View>{new View(m_Transport, client, revision.token(), revision.session(), revision.revision())};
	}

	View::View(std::shared_ptr<Transport> transport, std::string client, std::string token, std::string session, std::uint64_t revision)
		: m_Transport{std::move(transport)}, m_Client{std::move(client)}, m_Token{std::move(token)}, m_Session{std::move(session)}, m_Revision{revision}
	{
	}

	const std::string& View::Session() const
	{
		return m_Session;
	}
	std::uint64_t View::Revision() const
	{
		return m_Revision;
	}

	Error::Api::Result<std::vector<Units::Api::Id>> View::GetUnitInstanceIds() const
	{
		using Result = Error::Api::Result<std::vector<Units::Api::Id>>;
		auto response = m_Transport->Read(m_Client, m_Token, Query(Wire::Query::UNIT_IDS));
		if (!response)
		{
			return std::unexpected(Result::error_type{response.error()});
		}
		if (response->has_failure())
		{
			return DecodeFailure<Result>(response->failure());
		}
		try
		{
			if (!response->has_ids())
			{
				throw std::invalid_argument("unexpected_result");
			}
			std::vector<Units::Api::Id> result;
			for (const auto& id : response->ids().values())
			{
				result.push_back({id});
			}
			return result;
		}
		catch (const std::invalid_argument&)
		{
			return std::unexpected(Result::error_type{Error::Api::MakeError(Error::Api::ErrorCondition::ContractError)});
		}
	}

	Error::Api::Result<Units::Api::Instance> View::GetUnitInstance(const Units::Api::Id& id) const
	{
		using Result = Error::Api::Result<Units::Api::Instance>;
		auto response = m_Transport->Read(m_Client, m_Token, Query(Wire::Query::UNIT, id.Value));
		if (!response)
		{
			return std::unexpected(Result::error_type{response.error()});
		}
		if (response->has_failure())
		{
			return DecodeFailure<Result>(response->failure());
		}
		try
		{
			if (!response->has_unit())
			{
				throw std::invalid_argument("unexpected_result");
			}
			return Api::Decode(response->unit());
		}
		catch (const std::invalid_argument&)
		{
			return std::unexpected(Result::error_type{Error::Api::MakeError(Error::Api::ErrorCondition::ContractError)});
		}
	}

	Error::Api::Result<std::vector<Observations::Api::Id>> View::GetObservationIds() const
	{
		using Result = Error::Api::Result<std::vector<Observations::Api::Id>>;
		auto response = m_Transport->Read(m_Client, m_Token, Query(Wire::Query::OBSERVATION_IDS));
		if (!response)
		{
			return std::unexpected(Result::error_type{response.error()});
		}
		if (response->has_failure())
		{
			return DecodeFailure<Result>(response->failure());
		}
		try
		{
			if (!response->has_ids())
			{
				throw std::invalid_argument("unexpected_result");
			}
			std::vector<Observations::Api::Id> result;
			for (const auto& id : response->ids().values())
			{
				result.push_back({id});
			}
			return result;
		}
		catch (const std::invalid_argument&)
		{
			return std::unexpected(Result::error_type{Error::Api::MakeError(Error::Api::ErrorCondition::ContractError)});
		}
	}

	Error::Api::Result<Observations::Api::Instance> View::GetObservationInstance(const Observations::Api::Id& id) const
	{
		using Result = Error::Api::Result<Observations::Api::Instance>;
		auto response = m_Transport->Read(m_Client, m_Token, Query(Wire::Query::OBSERVATION, id.Value));
		if (!response)
		{
			return std::unexpected(Result::error_type{response.error()});
		}
		if (response->has_failure())
		{
			return DecodeFailure<Result>(response->failure());
		}
		try
		{
			if (!response->has_observation())
			{
				throw std::invalid_argument("unexpected_result");
			}
			return Api::Decode(response->observation());
		}
		catch (const std::invalid_argument&)
		{
			return std::unexpected(Result::error_type{Error::Api::MakeError(Error::Api::ErrorCondition::ContractError)});
		}
	}

	Error::Api::Result<Operations::Api::UnitState> View::GetUnitOperations(const Units::Api::Id& id) const
	{
		using Result = Error::Api::Result<Operations::Api::UnitState>;
		auto response = m_Transport->Read(m_Client, m_Token, Query(Wire::Query::OPERATIONS, id.Value));
		if (!response)
		{
			return std::unexpected(Result::error_type{response.error()});
		}
		if (response->has_failure())
		{
			return DecodeFailure<Result>(response->failure());
		}
		try
		{
			if (!response->has_operations())
			{
				throw std::invalid_argument("unexpected_result");
			}
			return Api::Decode(response->operations());
		}
		catch (const std::invalid_argument&)
		{
			return std::unexpected(Result::error_type{Error::Api::MakeError(Error::Api::ErrorCondition::ContractError)});
		}
	}

	Error::Api::Result<std::vector<Factions::Api::Id>> View::GetFactionIds()
	{
		using Result = Error::Api::Result<std::vector<Factions::Api::Id>>;
		auto response = m_Transport->Read(m_Client, m_Token, Query(Wire::Query::FACTION_IDS));
		if (!response)
		{
			return std::unexpected(Result::error_type{response.error()});
		}
		if (response->has_failure())
		{
			return DecodeFailure<Result>(response->failure());
		}
		try
		{
			if (!response->has_ids())
			{
				throw std::invalid_argument("unexpected_result");
			}
			std::vector<Factions::Api::Id> result;
			for (const auto& id : response->ids().values())
			{
				result.push_back({id});
			}
			return result;
		}
		catch (const std::invalid_argument&)
		{
			return std::unexpected(Result::error_type{Error::Api::MakeError(Error::Api::ErrorCondition::ContractError)});
		}
	}

	Error::Api::DomainResult<Classification::Api::Alignment, Factions::Api::Error> View::GetFactionAlignment(const Factions::Api::Id& id, const Factions::Api::Id& second)
	{
		using Result = Error::Api::DomainResult<Classification::Api::Alignment, Factions::Api::Error>;
		auto response = m_Transport->Read(m_Client, m_Token, Query(Wire::Query::FACTION_ALIGNMENT, id.Value, second.Value));
		if (!response)
		{
			return std::unexpected(Result::error_type{response.error()});
		}
		if (response->has_failure())
		{
			return DecodeFailure<Result>(response->failure());
		}
		try
		{
			if (!response->has_alignment())
			{
				throw std::invalid_argument("unexpected_result");
			}
			if (response->alignment() < 0 || response->alignment() > 2)
			{
				throw std::invalid_argument("invalid_alignment");
			}
			return static_cast<Classification::Api::Alignment>(response->alignment());
		}
		catch (const std::invalid_argument&)
		{
			return std::unexpected(Result::error_type{Error::Api::MakeError(Error::Api::ErrorCondition::ContractError)});
		}
	}

	Error::Api::Result<std::vector<Units::Api::TypeId>> View::GetUnitTypeIds()
	{
		using Result = Error::Api::Result<std::vector<Units::Api::TypeId>>;
		auto response = m_Transport->Read(m_Client, m_Token, Query(Wire::Query::UNIT_TYPE_IDS));
		if (!response)
		{
			return std::unexpected(Result::error_type{response.error()});
		}
		if (response->has_failure())
		{
			return DecodeFailure<Result>(response->failure());
		}
		try
		{
			if (!response->has_ids())
			{
				throw std::invalid_argument("unexpected_result");
			}
			std::vector<Units::Api::TypeId> result;
			for (const auto& id : response->ids().values())
			{
				result.push_back({id});
			}
			return result;
		}
		catch (const std::invalid_argument&)
		{
			return std::unexpected(Result::error_type{Error::Api::MakeError(Error::Api::ErrorCondition::ContractError)});
		}
	}

	Error::Api::Result<Units::Api::Type> View::GetUnitType(const Units::Api::TypeId& id)
	{
		using Result = Error::Api::Result<Units::Api::Type>;
		auto response = m_Transport->Read(m_Client, m_Token, Query(Wire::Query::UNIT_TYPE, id.Value));
		if (!response)
		{
			return std::unexpected(Result::error_type{response.error()});
		}
		if (response->has_failure())
		{
			return DecodeFailure<Result>(response->failure());
		}
		try
		{
			if (!response->has_unit_type())
			{
				throw std::invalid_argument("unexpected_result");
			}
			return Api::Decode(response->unit_type());
		}
		catch (const std::invalid_argument&)
		{
			return std::unexpected(Result::error_type{Error::Api::MakeError(Error::Api::ErrorCondition::ContractError)});
		}
	}

	Error::Api::Result<std::vector<Equipment::Api::TypeId>> View::GetEquipmentTypes()
	{
		using Result = Error::Api::Result<std::vector<Equipment::Api::TypeId>>;
		auto response = m_Transport->Read(m_Client, m_Token, Query(Wire::Query::EQUIPMENT_TYPE_IDS));
		if (!response)
		{
			return std::unexpected(Result::error_type{response.error()});
		}
		if (response->has_failure())
		{
			return DecodeFailure<Result>(response->failure());
		}
		try
		{
			if (!response->has_ids())
			{
				throw std::invalid_argument("unexpected_result");
			}
			std::vector<Equipment::Api::TypeId> result;
			for (const auto& id : response->ids().values())
			{
				result.push_back({id});
			}
			return result;
		}
		catch (const std::invalid_argument&)
		{
			return std::unexpected(Result::error_type{Error::Api::MakeError(Error::Api::ErrorCondition::ContractError)});
		}
	}

	Error::Api::DomainResult<Equipment::Api::Type, Equipment::Api::Error> View::GetEquipmentType(const Equipment::Api::TypeId& id) const
	{
		using Result = Error::Api::DomainResult<Equipment::Api::Type, Equipment::Api::Error>;
		auto response = m_Transport->Read(m_Client, m_Token, Query(Wire::Query::EQUIPMENT_TYPE, id.Value));
		if (!response)
		{
			return std::unexpected(Result::error_type{response.error()});
		}
		if (response->has_failure())
		{
			return DecodeFailure<Result>(response->failure());
		}
		try
		{
			if (!response->has_equipment_type())
			{
				throw std::invalid_argument("unexpected_result");
			}
			return Api::Decode(response->equipment_type());
		}
		catch (const std::invalid_argument&)
		{
			return std::unexpected(Result::error_type{Error::Api::MakeError(Error::Api::ErrorCondition::ContractError)});
		}
	}

	Error::Api::Result<std::vector<Resources::Api::Id>> View::GetResourceIds() const
	{
		using Result = Error::Api::Result<std::vector<Resources::Api::Id>>;
		auto response = m_Transport->Read(m_Client, m_Token, Query(Wire::Query::RESOURCE_IDS));
		if (!response)
		{
			return std::unexpected(Result::error_type{response.error()});
		}
		if (response->has_failure())
		{
			return DecodeFailure<Result>(response->failure());
		}
		try
		{
			if (!response->has_ids())
			{
				throw std::invalid_argument("unexpected_result");
			}
			std::vector<Resources::Api::Id> result;
			for (const auto& id : response->ids().values())
			{
				result.push_back({id});
			}
			return result;
		}
		catch (const std::invalid_argument&)
		{
			return std::unexpected(Result::error_type{Error::Api::MakeError(Error::Api::ErrorCondition::ContractError)});
		}
	}

	Error::Api::Result<std::vector<Resources::Api::Definition>> View::GetResourceDefinition(const Resources::Api::Id& id) const
	{
		using Result = Error::Api::Result<std::vector<Resources::Api::Definition>>;
		auto response = m_Transport->Read(m_Client, m_Token, Query(Wire::Query::RESOURCE_DEFINITIONS, id.Value));
		if (!response)
		{
			return std::unexpected(Result::error_type{response.error()});
		}
		if (response->has_failure())
		{
			return DecodeFailure<Result>(response->failure());
		}
		try
		{
			if (!response->has_resources())
			{
				throw std::invalid_argument("unexpected_result");
			}
			std::vector<Resources::Api::Definition> result;
			for (const auto& definition : response->resources().values())
			{
				result.push_back(Api::Decode(definition));
			}
			return result;
		}
		catch (const std::invalid_argument&)
		{
			return std::unexpected(Result::error_type{Error::Api::MakeError(Error::Api::ErrorCondition::ContractError)});
		}
	}

	Error::Api::Result<Physics::Api::Seconds> View::GetCurrentTime()
	{
		using Result = Error::Api::Result<Physics::Api::Seconds>;
		auto response = m_Transport->Read(m_Client, m_Token, Query(Wire::Query::TIME));
		if (!response)
		{
			return std::unexpected(Result::error_type{response.error()});
		}
		if (response->has_failure())
		{
			return DecodeFailure<Result>(response->failure());
		}
		try
		{
			if (!response->has_time_seconds())
			{
				throw std::invalid_argument("unexpected_result");
			}
			return Physics::Api::Seconds{response->time_seconds()};
		}
		catch (const std::invalid_argument&)
		{
			return std::unexpected(Result::error_type{Error::Api::MakeError(Error::Api::ErrorCondition::ContractError)});
		}
	}

	Error::Api::Result<bool> View::IsPaused() const
	{
		using Result = Error::Api::Result<bool>;
		auto response = m_Transport->Read(m_Client, m_Token, Query(Wire::Query::PAUSED));
		if (!response)
		{
			return std::unexpected(Result::error_type{response.error()});
		}
		if (response->has_failure())
		{
			return DecodeFailure<Result>(response->failure());
		}
		try
		{
			if (!response->has_boolean())
			{
				throw std::invalid_argument("unexpected_result");
			}
			return response->boolean();
		}
		catch (const std::invalid_argument&)
		{
			return std::unexpected(Result::error_type{Error::Api::MakeError(Error::Api::ErrorCondition::ContractError)});
		}
	}

	Error::Api::Result<bool> View::CanRequestPause() const
	{
		using Result = Error::Api::Result<bool>;
		auto response = m_Transport->Read(m_Client, m_Token, Query(Wire::Query::CAN_PAUSE));
		if (!response)
		{
			return std::unexpected(Result::error_type{response.error()});
		}
		if (response->has_failure())
		{
			return DecodeFailure<Result>(response->failure());
		}
		try
		{
			if (!response->has_boolean())
			{
				throw std::invalid_argument("unexpected_result");
			}
			return response->boolean();
		}
		catch (const std::invalid_argument&)
		{
			return std::unexpected(Result::error_type{Error::Api::MakeError(Error::Api::ErrorCondition::ContractError)});
		}
	}

	Error::Api::Result<bool> View::CanRequestUnpause() const
	{
		using Result = Error::Api::Result<bool>;
		auto response = m_Transport->Read(m_Client, m_Token, Query(Wire::Query::CAN_UNPAUSE));
		if (!response)
		{
			return std::unexpected(Result::error_type{response.error()});
		}
		if (response->has_failure())
		{
			return DecodeFailure<Result>(response->failure());
		}
		try
		{
			if (!response->has_boolean())
			{
				throw std::invalid_argument("unexpected_result");
			}
			return response->boolean();
		}
		catch (const std::invalid_argument&)
		{
			return std::unexpected(Result::error_type{Error::Api::MakeError(Error::Api::ErrorCondition::ContractError)});
		}
	}

	Error::Api::Result<Simulation::Api::TimeCompressionFactor> View::GetTimeCompression()
	{
		using Result = Error::Api::Result<Simulation::Api::TimeCompressionFactor>;
		auto response = m_Transport->Read(m_Client, m_Token, Query(Wire::Query::COMPRESSION));
		if (!response)
		{
			return std::unexpected(Result::error_type{response.error()});
		}
		if (response->has_failure())
		{
			return DecodeFailure<Result>(response->failure());
		}
		try
		{
			if (!response->has_compression())
			{
				throw std::invalid_argument("unexpected_result");
			}
			return Simulation::Api::TimeCompressionFactor{response->compression()};
		}
		catch (const std::invalid_argument&)
		{
			return std::unexpected(Result::error_type{Error::Api::MakeError(Error::Api::ErrorCondition::ContractError)});
		}
	}

	Error::Api::DomainResult<void, Simulation::Api::Error> View::RequestPause(bool paused)
	{
		Wire::Control control;
		control.set_session(m_Session);
		control.set_pause(paused);
		return m_Transport->Apply(m_Client, control);
	}

	Error::Api::DomainResult<void, Simulation::Api::Error> View::SetTimeCompression(Simulation::Api::TimeCompressionFactor factor)
	{
		Wire::Control control;
		control.set_session(m_Session);
		control.set_compression(factor.Value);
		return m_Transport->Apply(m_Client, control);
	}
}
