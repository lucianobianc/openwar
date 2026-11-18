# Connecticut Valley geographic scenario

This 32×32 km region is centred on Brattleboro, Vermont, and the Connecticut
River. The projected extent is eastings 682000–714000 m and northings
4730000–4762000 m in WGS84 / UTM zone 18N (EPSG:32618). The resolved world uses
16×16 strategic sectors and a 160×160 terrain lattice. The imported terrain
samples range from approximately 56 to 607 m above the supplied datum.

Both fictional scenarios use `valley-world.owgeo`, the same standalone
physical world. `valley-demo.owscenario` reproduces the original deployment,
with WEST and EAST outposts at sectors 122 and 175. The WEST route crosses the
Connecticut River and uses both roads and terrain. The original demonstration
ships 12 units of supplies to each outpost. `valley-autonomous.owscenario`
swaps the territorial corridors, changes the forces and supplies, and lets
both factions plan ground operations.

```sh
valley=examples/connecticut-valley
./openwar --inspect-world "$valley/valley-world.owgeo"
./openwar --scenario "$valley/valley-autonomous.owscenario" --ticks 40 --events
./openwar --scenario "$valley/valley-demo.owscenario" \
    --faction WEST --order defend:122 --ticks 120 --events
./openwar-gui --scenario "$valley/valley-autonomous.owscenario"
```

The original combined `connecticut-valley.owgeo` is still accepted by
`--scenario`.

## Source data and reproduction

- `valley.osm.pbf` is a fixed, filtered OSM extract obtained on 18 November 2026
  from the OSM map API. The rectangular source queries cover approximately
  72.79°W–72.36°W and 42.68°N–42.999°N. They were sorted, merged and filtered
  with osmium 1.18.0, retaining road and waterway topology, land-use features,
  and the aviation and transport catalog. **© OpenStreetMap contributors**,
  under ODbL 1.0. The OSM-derived content in the resolved world is also provided
  under ODbL 1.0. https://www.openstreetmap.org/copyright
- `valley-elevation.tif` is a cropped and 2× averaged copy of
  `Copernicus_DSM_COG_10_N42_00_W073_00_DEM.tif`. The GLO-30 source is a surface
  model with EGM2008 orthometric heights. **Copernicus DEM © DLR e.V. 2010–2014
  and © Airbus Defence and Space GmbH 2014–2018, provided under COPERNICUS by
  the European Union and ESA; all rights reserved.** The product's public
  licence applies to the source and derived elevation samples.
  https://copernicus-dem-30m.s3.amazonaws.com/readme.html

The fixed source inputs are bundled. The full source URLs, SHA-256 hashes,
projection, import settings and diagnostics are embedded in the resolved
world. Rebuild with GDAL 3.10.3 and osmium 1.18.0 for the checked-in output.

```sh
base=examples/connecticut-valley
python3 tools/import_geography.py --config "$base/valley-world.json" \
    --osm "$base/valley.osm.pbf" --dem "$base/valley-elevation.tif" \
    --output /tmp/valley-world.owgeo
cmp "$base/valley-world.owgeo" /tmp/valley-world.owgeo
```

Use `connecticut-valley.json` to reproduce the legacy combined demonstration.
