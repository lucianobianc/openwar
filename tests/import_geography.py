#!/usr/bin/env python3
"""Exercise the offline importer and native campaign loader with generated GIS inputs."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

import numpy as np
from osgeo import gdal, osr


def sources(directory):
    west, north = 500_000, 4_750_000
    projected = osr.SpatialReference()
    projected.ImportFromEPSG(32618)
    geographic = osr.SpatialReference()
    geographic.ImportFromEPSG(4326)
    geographic.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    transform = osr.CoordinateTransformation(projected, geographic)
    osm = ET.Element("osm", version="0.6")
    node_id, way_id = 0, 100

    def node(x, y, **tags):
        nonlocal node_id
        node_id += 1
        longitude, latitude, _ = transform.TransformPoint(west + x, north - y)
        item = ET.Element("node", id=str(node_id), lat=f"{latitude:.9f}", lon=f"{longitude:.9f}")
        for key, value in tags.items():
            ET.SubElement(item, "tag", k=key, v=value)
        osm.insert(node_id - 1, item)
        return str(node_id)

    def way(refs, **tags):
        nonlocal way_id
        item = ET.SubElement(osm, "way", id=str(way_id))
        for ref in refs:
            ET.SubElement(item, "nd", ref=ref)
        for key, value in tags.items():
            ET.SubElement(item, "tag", k=key, v=value)
        way_id += 1

    def rectangle(x0, y0, x1, y1, **tags):
        points = [node(x, y) for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1))]
        way(points + [points[0]], **tags)

    a, b, c, d = [node(x, 1000) for x in (1000, 1800, 2200, 3000)]
    way([a, b], highway="residential", surface="asphalt")
    way([b, c], highway="residential", bridge="yes", oneway="yes")
    way([c, d], highway="residential")
    e, f = [node(x, 1000) for x in (5000, 7000)]
    way([e, f], highway="secondary", surface="gravel")
    way([node(2000, 0), node(2000, 1999)], waterway="river")
    rectangle(1900, 100, 2100, 1900, natural="water")
    rectangle(400, 200, 800, 600, landuse="forest")
    rectangle(4000, 200, 4700, 600, landuse="industrial")
    rectangle(4850, 1200, 5200, 1500, aeroway="aerodrome", name="Sample aerodrome")
    way([node(4900, 1300), node(5150, 1300)], aeroway="runway", width="30", surface="asphalt")
    node(5050, 1400, aeroway="helipad", name='Nø "quotes"')
    way([node(1200, 750), node(1200, 1250)], highway="residential", bridge="yes", layer="1")
    osm_path = directory / "fixture.osm"
    ET.ElementTree(osm).write(osm_path, encoding="utf-8", xml_declaration=True)
    dem_path = directory / "fixture.tif"
    dem = gdal.GetDriverByName("GTiff").Create(str(dem_path), 82, 22, 1, gdal.GDT_Float32)
    dem.SetSpatialRef(projected)
    dem.SetGeoTransform((west - 50, 100, 0, north + 50, 0, -100))
    dem.GetRasterBand(1).WriteArray(np.tile(np.arange(82, dtype=np.float32) / 2, (22, 1)))
    dem = None
    config = {"version": 1, "epsg": 32618, "origin_m": [west, north], "columns": 4, "rows": 1,
              "sector_m": 2000, "tile_m": 100, "vertical_datum": "test datum",
              "sites": {"west_base": int(a), "west_outpost": int(d), "east_base": int(e), "east_outpost": int(f)},
              "sources": {kind: {"url": "test-fixture", "license": "CC0", "attribution": "Generated fixture"}
                          for kind in ("osm", "dem")}}
    config_path = directory / "fixture.json"
    config_path.write_text(json.dumps(config))
    return config_path, osm_path, dem_path


def main():
    osr.UseExceptions()
    engine = Path(sys.argv[1]).resolve()
    importer = Path(__file__).resolve().parents[1] / "tools/import_geography.py"
    with tempfile.TemporaryDirectory() as scratch:
        directory = Path(scratch)
        config, osm, dem = sources(directory)
        world = directory / "first.owgeo"
        copy = directory / "second.owgeo"

        def compile_to(output, check=True):
            return subprocess.run([sys.executable, importer, "--config", config, "--osm", osm,
                                   "--dem", dem, "--output", output], check=check, text=True,
                                  capture_output=not check)

        compile_to(world)
        compile_to(copy)
        assert world.read_bytes() == copy.read_bytes()
        lines = world.read_text(encoding="utf-8").splitlines()
        config_data = json.loads(config.read_text())
        physical_config = {key: value for key, value in config_data.items() if key != "sites"}
        physical_config["version"] = 2
        config.write_text(json.dumps(physical_config))
        compile_to(copy)
        physical_lines = copy.read_text(encoding="utf-8").splitlines()
        assert physical_lines[0] == "OWGEO 2"
        assert physical_lines[3:] == [line for line in lines[3:] if not line.startswith("site ")]
        assert "west_base" not in physical_lines[2]
        physical_run = subprocess.run([engine, "--inspect-world", copy], check=True, text=True, capture_output=True)
        assert "Geographic world: 4x1 sectors, 5 road segments" in physical_run.stdout
        config.write_text(json.dumps(config_data))
        sites = [line for line in lines if line.startswith("site ")]
        assert sites[:2] == ["site 1000000 1000000 5000", "site 3000000 1000000 15000"], sites
        assert "features 4" in lines and "roads 5" in lines
        assert any(line.startswith("1 1 ") and "250000 30000 0" in line for line in lines)
        save = directory / "campaign.ows"
        run = subprocess.run([engine, "--scenario", world, "--faction", "WEST", "--order", "defend:1",
                              "--ticks", "5", "--save", save], check=True, text=True, capture_output=True)
        assert "Imported geographic world: 4x1 sectors" in run.stdout
        run = subprocess.run([engine, "--load", save, "--faction", "WEST", "--events", "--ticks", "30"],
                             check=True, text=True, capture_output=True)
        assert "ground objective secured" in run.stdout, run.stdout
        observer = subprocess.run([engine, "--load", save, "--ticks", "30"],
                                  check=True, text=True, capture_output=True)
        assert "Shipments delivered: 2/2" in observer.stdout, observer.stdout

        bridge = next(i for i, line in enumerate(lines) if line.startswith("101 ") and len(line.split()) == 9)
        for column, value in ((4, "2"), (5, "0")):
            blocked = lines[:]
            fields = blocked[bridge].split()
            fields[column] = value
            blocked[bridge] = " ".join(fields)
            copy.write_text("\n".join(blocked) + "\n")
            failure = subprocess.run([engine, "--scenario", copy, "--ticks", "0"],
                                     text=True, capture_output=True)
            assert failure.returncode and "WEST deployment" in failure.stderr, failure.stderr

        config_data = json.loads(config.read_text())
        config_data["epsg"] = 4326
        config.write_text(json.dumps(config_data))
        failed = compile_to(copy, False)
        assert failed.returncode and "projected in metres" in failed.stderr, failed.stderr
    print("Offline GIS import, deterministic package, campaign movement and checkpoint resume passed.")


if __name__ == "__main__":
    main()
