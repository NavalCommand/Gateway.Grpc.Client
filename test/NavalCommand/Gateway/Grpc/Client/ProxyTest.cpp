#include <NavalCommand/Gateway/Grpc/Client/Connection.h>
#include <NavalCommand/Gateway/Grpc/MockProvider.h>
#include <NavalCommand/Gateway/Grpc/Server.h>
#include <gtest/gtest.h>
#include <thread>

namespace NavalCommand::Gateway::Grpc::Client
{
	class ProxyTest : public testing::Test
	{
	protected:
		MockProvider Provider;
		Server Host{Provider};
		std::unique_ptr<Connection> Remote;

		void SetUp() override
		{
			ASSERT_TRUE(Host.Start("127.0.0.1:0"));
			Remote = std::make_unique<Connection>("127.0.0.1:" + std::to_string(Host.Port()));
		}
	};

	TEST_F(ProxyTest, ServerScopesBlockEnemyTruthAndPreserveContactIdentity)
	{
		auto omniscient = Remote->Acquire();
		auto blue = Remote->Acquire(Factions::Api::Id{"blue"});
		auto red = Remote->Acquire(Factions::Api::Id{"red"});
		ASSERT_TRUE(omniscient);
		ASSERT_TRUE(blue);
		ASSERT_TRUE(red);
		EXPECT_EQ((*omniscient)->GetUnitInstanceIds()->size(), 2);
		EXPECT_EQ((*blue)->GetUnitInstanceIds()->size(), 1);
		auto enemy = (*blue)->GetUnitInstance({"truth-red"});
		ASSERT_FALSE(enemy);
		EXPECT_EQ(enemy.error().Condition, Error::Api::ErrorCondition::NotFound);
		EXPECT_FALSE((*blue)->GetUnitOperations({"truth-red"}));
		auto contact = (*blue)->GetObservationInstance({"contact-1"});
		ASSERT_TRUE(contact);
		EXPECT_EQ(contact->Identity.Value, "contact-1");
		EXPECT_EQ(contact->ObserverFaction.Value, "blue");
		EXPECT_FALSE(contact->ObservedFaction.IsKnown());
		EXPECT_FALSE(contact->Kinematics.Speed.IsKnown());
		EXPECT_EQ((*red)->GetObservationInstance({"contact-1"})->ObserverFaction.Value, "red");
	}

	TEST_F(ProxyTest, RetainedRevisionAndMissionInvalidation)
	{
		auto initial = Remote->Acquire();
		ASSERT_TRUE(initial);
		ASSERT_TRUE((*initial)->RequestPause(true));
		EXPECT_FALSE(*(*initial)->IsPaused());
		auto refreshed = Remote->Acquire();
		ASSERT_TRUE(refreshed);
		EXPECT_TRUE(*(*refreshed)->IsPaused());
		Provider.Reload("replacement-mission");
		auto expired = (*initial)->GetUnitInstanceIds();
		ASSERT_FALSE(expired);
		EXPECT_EQ(expired.error().Condition, Error::Api::ErrorCondition::NotReady);
		auto next = Remote->Acquire();
		ASSERT_TRUE(next);
		EXPECT_EQ((*next)->Session(), "replacement-mission");
		EXPECT_FALSE(*(*next)->IsPaused());
	}

	TEST_F(ProxyTest, PlayerAndClientPauseClaimsAreIndependent)
	{
		for (bool player : {false, true})
		{
			for (bool client : {false, true})
			{
				Provider.PlayerPause(player);
				auto view = Remote->Acquire();
				ASSERT_TRUE(view);
				ASSERT_TRUE((*view)->RequestPause(client));
				auto effective = Remote->Acquire();
				ASSERT_TRUE(effective);
				EXPECT_EQ(*(*effective)->IsPaused(), player || client);
			}
		}
		Provider.PlayerPause(false);
		auto view = Remote->Acquire();
		ASSERT_TRUE((*view)->RequestPause(true));
		Provider.PlayerPause(false);
		EXPECT_TRUE(*(*Remote->Acquire())->IsPaused());
	}

	TEST_F(ProxyTest, DisconnectReleasesOnlyItsOwnerAndReconnectStartsFresh)
	{
		Connection second{"127.0.0.1:" + std::to_string(Host.Port())};
		auto first = Remote->Acquire();
		auto other = second.Acquire();
		ASSERT_TRUE(first);
		ASSERT_TRUE(other);
		ASSERT_TRUE((*first)->RequestPause(true));
		ASSERT_TRUE((*other)->RequestPause(true));
		Remote->Stop();
		EXPECT_TRUE(*(*second.Acquire())->IsPaused());
		ASSERT_TRUE((*other)->RequestPause(false));
		bool paused = true;
		for (int attempt = 0; attempt < 100 && paused; ++attempt)
		{
			paused = *(*second.Acquire())->IsPaused();
			std::this_thread::sleep_for(std::chrono::milliseconds{10});
		}
		EXPECT_FALSE(paused);
		Provider.PlayerPause(true);
		second.Stop();
		Connection reconnected{"127.0.0.1:" + std::to_string(Host.Port())};
		auto fresh = reconnected.Acquire();
		ASSERT_TRUE(fresh);
		EXPECT_TRUE(*(*fresh)->IsPaused());
		Provider.PlayerPause(false);
		EXPECT_FALSE(*(*reconnected.Acquire())->IsPaused());
	}

	TEST_F(ProxyTest, CompressionLastAppliedWinsAndInvalidFactorPropagates)
	{
		auto view = Remote->Acquire();
		ASSERT_TRUE(view);
		Provider.PlayerCompression(2);
		ASSERT_TRUE((*view)->SetTimeCompression({4}));
		EXPECT_EQ((*Remote->Acquire())->GetTimeCompression()->Value, 4);
		Provider.PlayerCompression(1);
		EXPECT_EQ((*Remote->Acquire())->GetTimeCompression()->Value, 1);
		auto invalid = (*view)->SetTimeCompression({3});
		ASSERT_FALSE(invalid);
		EXPECT_EQ(std::get<Simulation::Api::Error>(invalid.error()), Simulation::Api::Error::InvalidTimeCompression);
	}

	TEST_F(ProxyTest, ShutdownAndServerRestartRecover)
	{
		const auto endpoint = "127.0.0.1:" + std::to_string(Host.Port());
		ASSERT_TRUE(Remote->Acquire());
		Host.Stop();
		EXPECT_FALSE(Remote->Acquire());
		ASSERT_TRUE(Host.Start(endpoint));
		bool recovered = false;
		for (int attempt = 0; attempt < 10 && !recovered; ++attempt)
		{
			recovered = Remote->Acquire().has_value();
		}
		EXPECT_TRUE(recovered);
	}

	TEST(ProxyTimeout, UnreachableEndpointIsAnErrorNotAnEmptyCollection)
	{
		Connection remote{"127.0.0.1:1", std::chrono::milliseconds{50}};
		const auto started = std::chrono::steady_clock::now();
		auto result = remote.Acquire();
		ASSERT_FALSE(result);
		EXPECT_EQ(result.error().Condition, Error::Api::ErrorCondition::CommunicationError);
		EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds{1});
	}
}
