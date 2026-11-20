# Korean geographic theater

This directory bundles two real geographic worlds and one fictional campaign.
Run the 48 × 48 km, 36-formations-per-faction pilot from the checkout root.

```sh
./openwar-gui --scenario examples/korean-theater/pilot-scale.owscenario
```

Keep `pilot-scale.owscenario` next to `pilot.owgeo` so its relative world
reference resolves. The 144 × 104 km world is available for inspection or
use from a separately authored campaign.

```sh
./openwar --inspect-world examples/korean-theater/inland.owgeo
```

The OWGEO 2 files embed their resolved geography and do not require GIS
tools or separate data downloads. This pilot measures the opening at
72 formations. The current default routing budget can prevent ground orders
on this road network.

Roads, bridges, waterways and land cover are © OpenStreetMap contributors,
distributed under the [Open Database License](https://www.openstreetmap.org/copyright).
The import used the [Geofabrik South Korea extract](https://download.geofabrik.de/asia/south-korea.html)
with replication timestamp 2026-11-19 21:22:20 UTC.

Elevation is [Copernicus DEM GLO-30](https://registry.opendata.aws/copernicus-dem/).
Copernicus DEM © DLR e.V. 2010–2014 and © Airbus Defence and Space GmbH
2014–2018, provided under COPERNICUS by the European Union and ESA; all rights
reserved. The adjacent JSON configurations and embedded OWGEO provenance
record the conversion settings and source attribution.
