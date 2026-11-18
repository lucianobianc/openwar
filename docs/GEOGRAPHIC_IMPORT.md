# Offline geographic import

OpenWar accepts resolved `.owgeo` scenarios through `--scenario`. The importer
uses GDAL/OGR and PROJ outside the game. The loader, simulation, and checkpoint
reader use C++20 and the existing routing engine.

## Import a region

Install Python 3.11 or later, GDAL's Python bindings with NumPy and PROJ, and
`osmium` for OSM PBF inputs. On Debian, the system Python can use the
`python3-gdal`, `python3-numpy`, and `osmium-tool` packages. An OSM XML or
gzip-compressed XML file can be used in place of PBF.

1. Obtain an OSM extract with nodes, ways, and multipolygon relations for the
   region. Download a single-band georeferenced DEM in metres covering the
   full region. Keep the downloaded snapshots and their licences.
2. Copy [the example configuration](../examples/connecticut-valley/connecticut-valley.json).
   Choose a local projected CRS in metres, preferably the region's UTM zone.
   Set the projected northwest origin, sector counts, and tile size. The
   default example uses 2 km sectors and 200 m terrain tiles. Tile size must
   divide sector size. The DEM should include padding of one terrain tile
   around the projected area.
3. Supply four distinct in-bounds deployment sites under `sites`. Each value
   is an OSM node ID or a WGS84 `[longitude, latitude]` array. Select a base and
   outpost per faction, in separate sectors, with accessible geographic routes
   and no overlap between the two initial territorial corridors. Choose
   fictional deployments independently of the geographic feature catalog.
4. Record the source URLs, licences, attribution, and vertical datum. Run the
   importer and then load the result to check the two deployment routes.

```sh
python3 tools/import_geography.py --config region.json --osm region.osm.pbf \
    --dem region.tif --output region.owgeo
./openwar --scenario region.owgeo --ticks 0
```

The loader reports malformed data, unavailable routes, and search-limit
failures separately. It places each faction's formation and carrier at its
base. Its verified route establishes the faction's initial territorial
corridor. The outpost is a strategic objective and the destination of an
initial shipment. A faction can issue a `defend:SECTOR` order to move its
formation to the outpost. The sandbox uses existing combat and logistics
rules and has a 24-hour time limit.

## Resolved world format

`OWGEO 1` is UTF-8 text. Records are separated by whitespace except for quoted
strings. Integers use decimal notation. Metric geometry uses signed
millimetres with east and south increasing from the northwest origin.

| Section | Contents |
|---|---|
| `OWGEO 1` | Geographic format signature and version |
| `grid` | Columns, rows, sector size in mm, terrain tile size in mm |
| `provenance` | Quoted JSON text containing source SHA-256 digests, licences, attribution, source and target CRS, origin, conversion settings and counts. Per-sector summaries give cover sample counts and minimum and maximum elevation in mm. |
| `tiles` | Row-major pairs of cover and elevation in mm. Cover codes are open 0, forest 1, water 2, urban 3. |
| `waterways COUNT` | Two local points per record. Elevation is zero for both endpoints. |
| `nodes COUNT` | OSM node ID followed by a local `east south up` point. Runtime IDs start at 1 in sorted source order. |
| `roads COUNT` | OSM way ID, runtime start and end node IDs, surface, direction, crossing, speed in mm/s, interior-access flag, waypoint count, then waypoint coordinates. Segments split at shared nodes. Surface codes are paved 0 and unpaved 1. Direction codes are both 0, forward 1, reverse 2. Crossing codes are ground 0 and bridge 1. |
| `features COUNT` | Kind, OSM element type, OSM ID, representative point, length and width in mm, area in m², then quoted name, surface and local-mm WKT. Zero dimensions indicate unavailable values. |
| `site` | Four local points in WEST base, WEST outpost, EAST base, EAST outpost order |
| `end` | End marker |

The four deployment records define this initial sandbox. The geographic
catalog belongs to the map and is also preserved in schema 4 checkpoints.
Older supported checkpoints retain their resolved journeys and load with the
current movement rules.

### Site candidates

`geography::Map::site_candidates(SiteKind)` returns catalog references of the
selected kind in source order. Kinds 0 to 8 are aerodrome, runway, helipad,
port, rail station, road junction, open land, industrial land, and urban land.
Source element codes are node 0, way 1, and relation 2. The stable reference
is the element type and original OSM ID, for example
`https://www.openstreetmap.org/way/ID`. Feature geometry and available widths,
lengths, surface tags, and areas describe mapped geography. The query performs
no suitability scoring. Scenario authors choose their own deployment points
and use the campaign's route validation to check them.

## Conversion and data-quality assumptions

- GDAL projects land-cover polygons and samples their classes at tile centres.
  Water takes precedence over urban and forest. Unclassified tiles use open
  cover, and urban tiles currently use the open-terrain mobility rate. OSM
  land-cover completeness varies. Use a separate water or coastal dataset
  before simulating areas where OSM shoreline lines leave the sea unclassified.
- Bilinear DEM samples are aligned to the runtime's tile lattice. Road and
  site heights are interpolated from those samples. Bridge deck heights are
  approximated between the segment endpoints. Missing or invalid DEM data
  rejects the import. The vertical datum is recorded as supplied.
- Roads connect through shared OSM node IDs. Intersections of projected lines
  do not form junctions. A way uses the recorded one-way direction and
  recognised surface and speed, or the defaults in the importer. Restricted
  motor access, conditional access, tunnels, and fords are omitted. Motorways,
  motor roads, and bridges use explicit road junctions for access. Unsupported
  conditional turns and turn-restriction relations are outside this version.
- Road runs stop where source nodes are missing, outside the import boundary,
  or behind a mapped restricted gate. Rivers and canals remain vector barriers
  independent of the terrain raster. Minor streams are not presumed
  impassable. Only source-tagged bridges receive bridge traversal.
- Geographic routing remains bounded. A regional limit indicates unresolved
  reachability. The benchmark compares limited queries against a larger fine
  search when the map size allows it.

## Validation tools

The main `make check` suite loads the bundled geographic scenario without
Python or GIS. The optional importer test generates small OSM and DEM fixtures
and tests projection, road topology, feature records, deterministic output,
runtime validation, and checkpoint continuation.

```sh
python3 tests/import_geography.py build/full/openwar
g++ -std=c++20 -O2 -Isrc tools/benchmark_geography.cpp build/full/libcampaign.a \
    -o /tmp/openwar-benchmark-geography
/tmp/openwar-benchmark-geography examples/connecticut-valley/connecticut-valley.owgeo
```
