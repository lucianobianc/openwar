#!/usr/bin/env python3
"""Compile offline OSM and elevation inputs into an OpenWar geographic scenario."""

import argparse
from collections import Counter
import gzip
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

import numpy as np
from osgeo import gdal, ogr, osr


VERSION = 1
SITES = ("west_base", "west_outpost", "east_base", "east_outpost")
ROAD_CLASSES = {
    "motorway": 90, "trunk": 70, "primary": 60, "secondary": 50,
    "tertiary": 40, "unclassified": 30, "residential": 30, "living_street": 20,
    "service": 20, "track": 15, "road": 20,
}
PAVED = {"asphalt", "concrete", "paved", "paving_stones", "sett", "cobblestone", "chipseal"}
UNPAVED = {"gravel", "fine_gravel", "compacted", "unpaved", "dirt", "ground", "grass",
           "sand", "mud", "earth", "pebblestone", "rock"}
RESTRICTED = {"no", "private", "destination", "customers", "permit", "agricultural", "forestry"}


def spatial_reference(epsg):
    crs = osr.SpatialReference()
    if crs.ImportFromEPSG(epsg) != 0:
        raise ValueError(f"unsupported EPSG:{epsg}")
    crs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    return crs


def read_config(path):
    config = json.loads(path.read_text(encoding="utf-8"))
    if config["version"] != VERSION:
        raise ValueError("unsupported import configuration version")
    columns, rows = (config[key] for key in ("columns", "rows"))
    sector, tile = (config[key] for key in ("sector_m", "tile_m"))
    if any(type(value) is not int for value in (columns, rows, sector, tile)):
        raise ValueError("grid dimensions and resolutions must be integers")
    if not (1 <= columns <= 128 and 1 <= rows <= 128 and 1 <= tile <= sector <= 100_000
            and sector % tile == 0 and columns * rows * (sector // tile) ** 2 <= 2_000_000):
        raise ValueError("invalid geographic grid or tile resolution")
    west, north = config["origin_m"]
    if not all(math.isfinite(value) for value in (west, north)):
        raise ValueError("invalid projected origin")
    if not config["vertical_datum"]:
        raise ValueError("a vertical datum description is required")
    if set(config["sites"]) != set(SITES):
        raise ValueError("four named deployment sites are required")
    for source in ("osm", "dem"):
        if not all(config["sources"][source][key] for key in ("url", "license", "attribution")):
            raise ValueError(f"{source} provenance requires url, license and attribution")
    return config


def road_attributes(tags, counters):
    highway = tags.get("highway", "")
    kind = highway.removesuffix("_link")
    if kind not in ROAD_CLASSES or tags.get("area") == "yes":
        return None
    access = next((tags[key] for key in ("motorcar", "motor_vehicle", "vehicle", "access") if key in tags), "")
    conditional = any(key in tags for key in
                      ("motorcar:conditional", "motor_vehicle:conditional", "vehicle:conditional",
                       "access:conditional", "oneway:conditional"))
    if (access in RESTRICTED or conditional or tags.get("tunnel", "no") != "no"
            or tags.get("ford", "no") != "no"):
        counters["restricted_ways_omitted"] += 1
        return None
    oneway = tags.get("oneway:motor_vehicle", tags.get("oneway"))
    if oneway is None:
        counters["direction_defaults"] += 1
        oneway = "yes" if tags.get("junction") == "roundabout" or highway.startswith("motorway") else "no"
    directions = {"yes": 1, "1": 1, "true": 1, "-1": 2, "reverse": 2, "no": 0, "0": 0, "false": 0}
    if oneway not in directions:
        counters["unsupported_directions_omitted"] += 1
        return None
    surface = tags.get("surface")
    if surface not in PAVED and surface not in UNPAVED:
        counters["surface_defaults"] += 1
        surface = "unpaved" if kind == "track" else "paved"
    speed_tag = tags.get("maxspeed", "").strip()
    match = re.fullmatch(r"([1-9][0-9]{0,2})(?:\s*(mph|km/h))?", speed_tag)
    if match:
        speed = int(match[1]) * (1.609344 if match[2] == "mph" else 1)
    else:
        counters["speed_defaults"] += 1
        speed = ROAD_CLASSES[kind]
    bridge = tags.get("bridge", "no") not in ("no", "")
    interior_access = not (bridge or kind == "motorway" or tags.get("motorroad") == "yes")
    return (int(surface not in PAVED), directions[oneway], int(bridge),
            min(50_000, round(speed * 1000 / 3.6)), int(interior_access))


def site_kind(tags):
    aviation = {"aerodrome": 0, "runway": 1, "helipad": 2}
    if tags.get("aeroway") in aviation:
        return aviation[tags["aeroway"]]
    if tags.get("landuse") in ("port", "harbour") or tags.get("harbour") not in (None, "no"):
        return 3
    if tags.get("railway") in ("station", "halt", "terminal") or tags.get("public_transport") == "station":
        return 4
    if tags.get("highway") == "motorway_junction":
        return 5
    if tags.get("landuse") in ("farmland", "meadow", "grass", "greenfield", "brownfield"):
        return 6
    if tags.get("landuse") in ("industrial", "railway"):
        return 7
    if tags.get("landuse") in ("commercial", "residential", "retail"):
        return 8
    return None


def dimension_mm(tag):
    match = re.fullmatch(r"(\d+(?:\.\d+)?)\s*(m|ft)?", tag or "")
    if not match:
        return 0
    return min(1_000_000_000, round(float(match[1]) * (304.8 if match[2] == "ft" else 1000)))


def project_geometry(geometry, west, north):
    for index in range(geometry.GetPointCount()):
        east, south, *_ = geometry.GetPoint(index)
        geometry.SetPoint_2D(index, round((east - west) * 1000), round((north - south) * 1000))
    for index in range(geometry.GetGeometryCount()):
        project_geometry(geometry.GetGeometryRef(index), west, north)


def catalog_features(candidates, positions, relation_shapes, width, height, point, counters):
    box = ogr.CreateGeometryFromWkt(f"POLYGON ((0 0,{width-1} 0,{width-1} {height-1},0 {height-1},0 0))")
    result = []
    for element, identity, kind, tags, refs in sorted(candidates, key=lambda entry: entry[:2]):
        if identity <= 0:
            continue
        if element == 0:
            geometry = ogr.Geometry(ogr.wkbPoint)
            geometry.AddPoint_2D(*positions[identity])
        elif element == 2:
            geometry = relation_shapes.get(identity)
            if geometry is None:
                counters["unresolved_site_relations"] += 1
                continue
        else:
            if len(refs) < 2 or any(ref not in positions for ref in refs):
                counters["unresolved_site_ways"] += 1
                continue
            if len(refs) >= 4 and refs[0] == refs[-1]:
                ring = ogr.Geometry(ogr.wkbLinearRing)
                for ref in refs:
                    ring.AddPoint_2D(*positions[ref])
                geometry = ogr.Geometry(ogr.wkbPolygon)
                geometry.AddGeometry(ring)
                if not geometry.IsValid():
                    geometry = geometry.MakeValid()
            else:
                geometry = ogr.Geometry(ogr.wkbLineString)
                for ref in refs:
                    geometry.AddPoint_2D(*positions[ref])
        if not geometry or not geometry.Intersects(box):
            continue
        shape = geometry.Intersection(box)
        if shape.IsEmpty():
            continue
        try:
            location = shape.PointOnSurface()
        except RuntimeError:
            location = shape.Centroid()
        if location.IsEmpty():
            continue
        east, south = (round(value) for value in location.GetPoint_2D())
        if not (0 <= east < width and 0 <= south < height):
            continue
        area = round(shape.GetArea() / 1_000_000) if shape.GetDimension() == 2 else 0
        length = dimension_mm(tags.get("length"))
        if not length and kind == 1 and shape.GetDimension() == 1:
            length = round(shape.Length())
        outline = shape.ExportToWkt()
        if len(outline) > 512 * 1024:
            counters["oversized_site_geometry_omitted"] += 1
            outline = ""
        result.append((kind, element, identity, *point(east, south), length,
                       dimension_mm(tags.get("width")), area,
                       tags.get("name", "")[:250], tags.get("surface", "")[:30], outline))
    return result


def read_osm(path):
    opener = gzip.open if path.suffix == ".gz" else open
    nodes, roads, waterways, barriers = {}, [], [], set()
    counters = Counter()
    candidates = []

    def node(element):
        identity = int(element.attrib["id"])
        lon, lat = float(element.attrib["lon"]), float(element.attrib["lat"])
        if identity in nodes or not (-180 <= lon <= 180 and -90 <= lat <= 90):
            raise ValueError(f"duplicate OSM node or invalid coordinates at node {identity}")
        nodes[identity] = (lon, lat)
        tags = {tag.attrib["k"]: tag.attrib["v"] for tag in element.findall("tag")}
        kind = site_kind(tags)
        if kind is not None:
            candidates.append((0, identity, kind, tags, []))
        if (tags.get("barrier") in {"bollard", "block", "gate", "lift_gate", "chain", "wall"}
                and next((tags[key] for key in ("motorcar", "motor_vehicle", "vehicle", "access")
                          if key in tags), "") not in {"yes", "permissive", "designated"}):
            barriers.add(identity)

    with opener(path, "rb") as stream:
        events = ET.iterparse(stream, ("start", "end"))
        _, root = next(events)
        if root.tag != "osm":
            raise ValueError("expected OSM XML root")
        for event, element in events:
            if event != "end":
                continue
            if element.tag == "node":
                node(element)
            elif element.tag == "way":
                tags = {tag.attrib["k"]: tag.attrib["v"] for tag in element.findall("tag")}
                refs = [int(ref.attrib["ref"]) for ref in element.findall("nd")]
                kind = site_kind(tags)
                if kind is not None:
                    candidates.append((1, int(element.attrib["id"]), kind, tags, refs))
                attrs = road_attributes(tags, counters)
                if attrs is not None:
                    roads.append((int(element.attrib["id"]), refs, attrs))
                if (tags.get("waterway") in ("river", "canal") and tags.get("tunnel", "no") == "no"
                        and tags.get("covered") != "yes"):
                    waterways.append((int(element.attrib["id"]), refs))
            elif element.tag == "relation":
                tags = {tag.attrib["k"]: tag.attrib["v"] for tag in element.findall("tag")}
                kind = site_kind(tags)
                if kind is not None:
                    candidates.append((2, int(element.attrib["id"]), kind, tags, []))
            elif element.tag != "bounds":
                continue
            root.remove(element)
    if not nodes or not roads:
        raise ValueError("OSM data contains no nodes or supported roads")
    if len({entry[0] for entry in roads}) != len(roads):
        raise ValueError("duplicate OSM road way ID")
    return nodes, sorted(roads), sorted(waterways), barriers, candidates, counters


def warp_dem(path, crs, bounds, width, height):
    source = gdal.Open(str(path))
    if source is None or source.RasterCount != 1 or source.GetSpatialRef() is None:
        raise ValueError("expected a single-band DEM with source CRS")
    source_crs = source.GetSpatialRef()
    if not (source_crs.IsGeographic() or source_crs.IsProjected()):
        raise ValueError("unsupported DEM horizontal CRS")
    band = source.GetRasterBand(1)
    if (band.GetUnitType().lower() not in ("", "m", "metre", "meter", "metres", "meters")
            or band.GetScale() not in (None, 1) or band.GetOffset() not in (None, 0)):
        raise ValueError("DEM samples must be unscaled elevations in metres")
    # Project only the horizontal CRS. The supplied vertical datum is retained as provenance.
    dem = gdal.Warp("", source, format="MEM", dstSRS=crs.ExportToWkt(), outputBounds=bounds,
                    width=width, height=height, resampleAlg="bilinear", dstNodata=float("nan"),
                    outputType=gdal.GDT_Float64, errorThreshold=0,
                    warpOptions=["NUM_THREADS=1", "APPLY_VERTICAL_SHIFT=NO"])
    data = dem.ReadAsArray()
    if not np.isfinite(data).all() or np.any((data < -20_000) | (data > 30_000)):
        raise ValueError("DEM has missing or invalid elevation within the requested extent")
    return np.rint(data * 1000).astype(np.int64), source_crs.ExportToWkt()


def cover_code(feature):
    natural = feature.GetField("natural")
    landuse = feature.GetField("landuse")
    if natural in ("water", "bay", "strait") or landuse in ("reservoir", "basin") or feature.GetField("waterway") == "riverbank":
        return 2
    if natural == "wood" or landuse == "forest":
        return 1
    if landuse in ("residential", "commercial", "industrial", "retail") or feature.GetField("building"):
        return 3
    return 0


def ground_cover(osm_path, crs, bounds, width, height, relation_ids, counters):
    # Configure OGR's OSM driver to retain the polygon tags used above.
    osm_config = Path(gdal.FindFile("gdal", "osmconf.ini")).read_text(encoding="utf-8")
    config = osm_config.replace("attributes=name,type,aeroway", "attributes=name,type,waterway,aeroway")
    config_path = "/vsimem/openwar-osmconf.ini"
    gdal.FileFromMemBuffer(config_path, config.encode())
    dataset = gdal.OpenEx(str(osm_path), gdal.OF_VECTOR, open_options=[f"CONFIG_FILE={config_path}"])
    if dataset is None:
        raise ValueError("OGR could not open OSM land cover")
    layer = dataset.GetLayerByName("multipolygons")
    if layer is None:
        raise ValueError("OSM dataset has no multipolygon layer")
    transform = osr.CoordinateTransformation(spatial_reference(4326), crs)
    vector = ogr.GetDriverByName("Memory").CreateDataSource("")
    polygons = vector.CreateLayer("cover", srs=crs)
    polygons.CreateField(ogr.FieldDefn("code", ogr.OFTInteger))
    groups = {1: [], 2: [], 3: []}
    relations = {}
    for feature in layer:
        code = cover_code(feature)
        relation = int(feature.GetField("osm_id") or 0)
        if (not code and relation not in relation_ids) or not feature.GetGeometryRef():
            continue
        geometry = feature.GetGeometryRef().Clone()
        geometry.Transform(transform)
        west, east, south, north = geometry.GetEnvelope()
        if east < bounds[0] or west > bounds[2] or north < bounds[1] or south > bounds[3]:
            continue
        if not geometry.IsValid():
            counters["cover_polygons_repaired"] += 1
            geometry = geometry.MakeValid()
        if geometry and not geometry.IsEmpty():
            if relation in relation_ids:
                relations[relation] = geometry.Clone()
            if code:
                groups[code].append(geometry)
    for code in (1, 3, 2):  # Water takes precedence over urban and forest cover.
        for geometry in groups[code]:
            feature = ogr.Feature(polygons.GetLayerDefn())
            feature.SetField("code", code)
            feature.SetGeometry(geometry)
            polygons.CreateFeature(feature)
    raster = gdal.GetDriverByName("MEM").Create("", width, height, 1, gdal.GDT_Byte)
    west, south, east, north = bounds
    raster.SetGeoTransform((west, (east - west) / width, 0, north, 0, (south - north) / height))
    raster.SetSpatialRef(crs)
    gdal.RasterizeLayer(raster, [1], polygons, options=["ATTRIBUTE=code"])
    gdal.Unlink(config_path)
    return raster.ReadAsArray(), relations


def road_graph(roads, projected, barriers, counters):
    runs = []
    for identity, refs, attrs in roads:
        run = []
        for node in refs + [None]:
            if node in projected and node not in barriers:
                if run and projected[run[-1]][:2] == projected[node][:2]:
                    # Duplicate geometry keeps its distinct OSM connectivity.
                    if run[-1] == node:
                        continue
                    counters["coincident_road_nodes"] += 1
                    if len(run) >= 2:
                        runs.append((identity, run, attrs))
                    run = [node]
                else:
                    run.append(node)
                continue
            if len(run) >= 2:
                runs.append((identity, run, attrs))
            if node is not None:
                counters["road_vertices_outside_or_restricted"] += 1
            run = []
    uses = Counter(node for _, run, _ in runs for node in run)
    endpoints = {node for _, run, _ in runs for node in (run[0], run[-1])}
    junctions = endpoints | {node for node, count in uses.items() if count > 1}
    nodes = sorted(junctions)
    ids = {node: i for i, node in enumerate(nodes, 1)}
    segments = []
    for identity, run, attrs in runs:
        start = 0
        for end in range(1, len(run)):
            if run[end] not in junctions:
                continue
            intermediate = [projected[node] for node in run[start + 1:end]]
            if attrs[2] and intermediate:
                # A DSM samples the ground or water below bridges. Approximate the deck from its ends.
                z0 = projected[run[start]][2]
                z1 = projected[run[end]][2]
                chain = [projected[node] for node in run[start:end + 1]]
                lengths = [math.dist(a[:2], b[:2]) for a, b in zip(chain, chain[1:])]
                total = sum(lengths)
                offset = 0
                for index, (east, south, _) in enumerate(intermediate):
                    offset += lengths[index]
                    intermediate[index] = east, south, round(z0 + (z1 - z0) * offset / total)
            if identity > 0:
                segments.append((identity, ids[run[start]], ids[run[end]], attrs, intermediate))
            start = end
    return nodes, segments


def waterway_segments(ways, positions, width, height, counters):
    box = ogr.CreateGeometryFromWkt(f"POLYGON ((0 0,{width-1} 0,{width-1} {height-1},0 {height-1},0 0))")
    segments = set()
    for _, refs in ways:
        for a, b in zip(refs, refs[1:]):
            if a not in positions or b not in positions:
                counters["missing_waterway_vertices"] += 1
                continue
            line = ogr.Geometry(ogr.wkbLineString)
            line.AddPoint_2D(*positions[a])
            line.AddPoint_2D(*positions[b])
            if not line.Intersects(box):
                continue
            part = line.Intersection(box)
            if part.GetGeometryName() != "LINESTRING":
                continue
            start = tuple(round(value) for value in part.GetPoint_2D(0))
            end = tuple(round(value) for value in part.GetPoint_2D(part.GetPointCount() - 1))
            if start != end:
                segments.add(tuple(sorted((start, end))))
    return sorted(segments)


def sector_summaries(cover, elevation, config):
    stride = config["sector_m"] // config["tile_m"]
    summaries = []
    for row in range(config["rows"]):
        for col in range(config["columns"]):
            area = np.s_[row * stride:(row + 1) * stride, col * stride:(col + 1) * stride]
            height = elevation[area]
            summaries.append([np.bincount(cover[area].ravel(), minlength=4).tolist(),
                              int(height.min()), int(height.max())])
    return summaries


def local_points(nodes, transform, west, north):
    identities = sorted(nodes)
    transformed = transform.TransformPoints([nodes[identity] for identity in identities])
    return {identity: (round((east - west) * 1000), round((north - south) * 1000))
            for identity, (east, south, *_) in zip(identities, transformed)}


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def compile_world(config, osm_xml, osm_dataset, osm_input, dem_path, output):
    crs = spatial_reference(config["epsg"])
    if not crs.IsProjected() or not math.isclose(crs.GetLinearUnits(), 1):
        raise ValueError("target CRS must be projected in metres")
    transform = osr.CoordinateTransformation(spatial_reference(4326), crs)
    west, north = config["origin_m"]
    width_m = config["columns"] * config["sector_m"]
    height_m = config["rows"] * config["sector_m"]
    width_mm, height_mm = width_m * 1000, height_m * 1000
    tile_mm = config["tile_m"] * 1000
    width, height = width_mm // tile_mm, height_mm // tile_mm
    bounds = west, north - height_m, west + width_m, north
    nodes, roads, waterways, barriers, candidates, counts = read_osm(osm_xml)
    positions = local_points(nodes, transform, west, north)
    half = config["tile_m"] / 2
    elevation_bounds = (bounds[0] - half, bounds[1] + half, bounds[2] - half, bounds[3] + half)
    elevation, source_crs = warp_dem(dem_path, crs, elevation_bounds, width, height)
    cover, relation_shapes = ground_cover(osm_dataset, crs, bounds, width, height,
        {identity for element, identity, *_ in candidates if element == 2}, counts)
    for shape in relation_shapes.values():
        project_geometry(shape, west, north)

    def point(east, south):
        x, dx = divmod(east, tile_mm)
        y, dy = divmod(south, tile_mm)
        x_next, y_next = min(x + 1, width - 1), min(y + 1, height - 1)
        # Interpolate the same samples used by the runtime lattice.
        top = (tile_mm - dx) * int(elevation[y, x]) + dx * int(elevation[y, x_next])
        bottom = (tile_mm - dx) * int(elevation[y_next, x]) + dx * int(elevation[y_next, x_next])
        return east, south, round(((tile_mm - dy) * top + dy * bottom) / tile_mm ** 2)

    projected = {identity: point(east, south) for identity, (east, south) in positions.items()
                 if 0 <= east < width_mm and 0 <= south < height_mm}
    node_ids, segments = road_graph(roads, projected, barriers, counts)
    rivers = waterway_segments(waterways, positions, width_mm, height_mm, counts)
    features = catalog_features(candidates, positions, relation_shapes, width_mm, height_mm, point, counts)
    if not node_ids or not segments:
        raise ValueError("no connected road sections inside the projected extent")
    sites = []
    for name in SITES:
        chosen = config["sites"][name]
        if type(chosen) is int:
            if chosen not in projected or chosen in barriers:
                raise ValueError(f"{name} node is outside the extent or restricted")
            sites.append(projected[chosen])
        else:
            if (not isinstance(chosen, list) or len(chosen) != 2 or
                    not all(type(value) in (float, int) and math.isfinite(value) for value in chosen) or
                    not (-180 <= chosen[0] <= 180 and -90 <= chosen[1] <= 90)):
                raise ValueError(f"{name} requires a node ID or WGS84 longitude and latitude")
            east, south = local_points({1: chosen}, transform, west, north)[1]
            if not (0 <= east < width_mm and 0 <= south < height_mm):
                raise ValueError(f"{name} coordinate outside extent")
            sites.append(point(east, south))
    metadata = dict(config)
    metadata.update({"importer": VERSION, "gdal": gdal.VersionInfo("RELEASE_NAME"),
                     "target_wkt": crs.ExportToWkt(), "dem_source_wkt": source_crs,
                     "source_sha256": {"osm": digest(osm_input), "dem": digest(dem_path)},
                     "sampling": "bilinear DEM; pixel-centre land cover",
                     "defaults": dict(sorted(counts.items())),
                     "sector_summaries": sector_summaries(cover, elevation, config)})
    content = json.dumps(metadata, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n",
                                         dir=output.parent, delete=False) as file:
            temporary = Path(file.name)
            def emit(*items):
                print(*items, file=file)

            emit("OWGEO", VERSION)
            emit("grid", config["columns"], config["rows"], config["sector_m"] * 1000, tile_mm)
            emit("provenance", json.dumps(content))
            emit("tiles")
            for colors, heights in zip(cover, elevation):
                emit(" ".join(f"{color} {altitude}" for color, altitude in zip(colors, heights)))
            emit("waterways", len(rivers))
            for (a, b), (c, d) in rivers:
                emit(a, b, 0, c, d, 0)
            emit("nodes", len(node_ids))
            for identity in node_ids:
                emit(identity, *projected[identity])
            emit("roads", len(segments))
            for identity, a, b, attrs, intermediate in segments:
                emit(identity, a, b, *attrs, len(intermediate))
                for vertex in intermediate:
                    emit(*vertex)
            emit("features", len(features))
            for feature in features:
                emit(*feature[:-3], *(json.dumps(value, ensure_ascii=False) for value in feature[-3:]))
            for site in sites:
                emit("site", *site)
            emit("end")
        temporary.replace(output)
    finally:
        if temporary:
            temporary.unlink(missing_ok=True)
    print(f"{output}: {width}×{height} tiles, {len(node_ids)} nodes, {len(segments)} roads, {len(rivers)} waterway sections")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("config", "osm", "dem", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    gdal.UseExceptions()
    ogr.UseExceptions()
    osr.UseExceptions()
    # Coordinate transformations use the installed CRS database and local grids only.
    osr.SetPROJEnableNetwork(False)
    try:
        config = read_config(args.config)
        with tempfile.TemporaryDirectory(prefix="openwar-geography-") as temp:
            osm_xml = args.osm
            osm_dataset = args.osm
            if args.osm.suffix == ".pbf":
                osm_xml = Path(temp) / "input.osm"
                subprocess.run(["osmium", "cat", str(args.osm), "-o", str(osm_xml)], check=True)
            elif args.osm.suffix == ".gz":
                osm_dataset = osm_xml = Path(temp) / "input.osm"
                with gzip.open(args.osm, "rb") as source, osm_xml.open("wb") as target:
                    while chunk := source.read(1024 * 1024):
                        target.write(chunk)
            compile_world(config, osm_xml, osm_dataset, args.osm, args.dem, args.output)
    except (OSError, RuntimeError, ValueError, TypeError, KeyError, ET.ParseError, subprocess.CalledProcessError) as error:
        print(f"openwar geographic import: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
