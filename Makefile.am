AM_CPPFLAGS = -I$(srcdir)/src
AM_CXXFLAGS = -std=c++20

noinst_LIBRARIES = libcampaign.a
libcampaign_a_SOURCES = src/campaign.cpp src/campaign.hpp src/economy.cpp \
	src/logistics.cpp src/forces.cpp src/scenario.cpp src/scenario.hpp src/scenario_config.cpp src/geographic_scenario.cpp src/intelligence.cpp \
	src/operations.cpp src/operations.hpp src/strategy.cpp src/combat.cpp src/combat.hpp src/territory.cpp src/persistence.cpp \
	src/geographic_movement.cpp src/geographic_movement.hpp

bin_PROGRAMS = openwar
openwar_SOURCES = src/main.cpp src/realtime.cpp src/realtime.hpp
openwar_LDADD = libcampaign.a

check_PROGRAMS = campaign-tests geographic-movement-tests
campaign_tests_SOURCES = tests/campaign.cpp
campaign_tests_LDADD = libcampaign.a
geographic_movement_tests_SOURCES = tests/geographic_movement.cpp
geographic_movement_tests_LDADD = libcampaign.a

AM_TESTS_ENVIRONMENT = OPENWAR=./openwar$(EXEEXT); export OPENWAR; \
	OWGEO_FIXTURE="$(srcdir)/examples/connecticut-valley/connecticut-valley.owgeo"; export OWGEO_FIXTURE; \
	VALLEY_FIXTURES="$(srcdir)/examples/connecticut-valley"; export VALLEY_FIXTURES;
TESTS = campaign-tests$(EXEEXT) tests/cli.sh geographic-movement-tests$(EXEEXT)

if BUILD_VIEWER
bin_PROGRAMS += openwar-gui
openwar_gui_SOURCES = src/viewer.cpp src/viewer_model.cpp src/viewer_model.hpp \
	src/viewer_geography.cpp src/viewer_geography.hpp
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
	docs/CAMPAIGN_SETUP.md docs/GEOGRAPHIC_WORLD.md docs/COMMAND_INTERFACE.md \
	examples/procedural.toml docs/GEOGRAPHIC_IMPORT.md tests/import_geography.py \
	tools/import_geography.py tools/benchmark_geography.cpp \
	examples/connecticut-valley/README.md examples/connecticut-valley/connecticut-valley.json \
	examples/connecticut-valley/connecticut-valley.owgeo examples/connecticut-valley/valley.osm.pbf \
	examples/connecticut-valley/valley-elevation.tif \
	examples/connecticut-valley/valley-world.json examples/connecticut-valley/valley-world.owgeo \
	examples/connecticut-valley/valley-demo.owscenario examples/connecticut-valley/valley-autonomous.owscenario
