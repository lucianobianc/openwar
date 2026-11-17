AM_CPPFLAGS = -I$(srcdir)/src
AM_CXXFLAGS = -std=c++20

noinst_LIBRARIES = libcampaign.a
libcampaign_a_SOURCES = src/campaign.cpp src/campaign.hpp src/economy.cpp src/scenario.cpp

bin_PROGRAMS = openwar
openwar_SOURCES = src/main.cpp
openwar_LDADD = libcampaign.a

check_PROGRAMS = campaign-tests
campaign_tests_SOURCES = tests/campaign.cpp
campaign_tests_LDADD = libcampaign.a

AM_TESTS_ENVIRONMENT = OPENWAR=./openwar$(EXEEXT); export OPENWAR;
TESTS = campaign-tests$(EXEEXT) tests/cli.sh
EXTRA_DIST = tests/cli.sh AGENTS.md LICENSE \
	docs/DESIGN.md docs/TERRITORY.md docs/ANNEXES_README.md \
	docs/ECONOMY.md docs/LOGISTICS.md docs/MAINTENANCE.md \
	docs/MILITARY_FORCES.md docs/INTELLIGENCE.md docs/OPERATIONS.md \
	docs/COMBAT.md docs/TACTICAL.md docs/SCENARIOS.md \
	docs/CAMPAIGN_SETUP.md
