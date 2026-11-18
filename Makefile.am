AM_CPPFLAGS = -I$(srcdir)/src
AM_CXXFLAGS = -std=c++20

noinst_LIBRARIES = libcampaign.a
libcampaign_a_SOURCES = src/campaign.cpp src/campaign.hpp src/economy.cpp \
	src/logistics.cpp src/forces.cpp src/scenario.cpp src/scenario.hpp src/scenario_config.cpp src/intelligence.cpp \
	src/operations.cpp src/operations.hpp src/strategy.cpp src/combat.cpp src/combat.hpp src/territory.cpp

bin_PROGRAMS = openwar
openwar_SOURCES = src/main.cpp src/realtime.cpp src/realtime.hpp
openwar_LDADD = libcampaign.a

check_PROGRAMS = campaign-tests
campaign_tests_SOURCES = tests/campaign.cpp
campaign_tests_LDADD = libcampaign.a

AM_TESTS_ENVIRONMENT = OPENWAR=./openwar$(EXEEXT); export OPENWAR;
TESTS = campaign-tests$(EXEEXT) tests/cli.sh

if BUILD_VIEWER
bin_PROGRAMS += openwar-gui
openwar_gui_SOURCES = src/viewer.cpp src/viewer_model.cpp src/viewer_model.hpp
openwar_gui_CPPFLAGS = $(AM_CPPFLAGS) $(RAYLIB_CFLAGS)
openwar_gui_LDADD = libcampaign.a $(RAYLIB_LIBS)

check_PROGRAMS += viewer-model-tests
viewer_model_tests_SOURCES = tests/viewer_model.cpp src/viewer_model.cpp src/viewer_model.hpp
viewer_model_tests_LDADD = libcampaign.a
TESTS += viewer-model-tests$(EXEEXT)
else
AM_DISTCHECK_CONFIGURE_FLAGS = --disable-viewer
endif

EXTRA_DIST = tests/cli.sh AGENTS.md LICENSE \
	docs/DESIGN.md docs/TERRITORY.md docs/ANNEXES_README.md \
	docs/ECONOMY.md docs/LOGISTICS.md docs/MAINTENANCE.md \
	docs/MILITARY_FORCES.md docs/INTELLIGENCE.md docs/OPERATIONS.md \
	docs/COMBAT.md docs/TACTICAL.md docs/SCENARIOS.md \
	docs/CAMPAIGN_SETUP.md examples/procedural.toml
