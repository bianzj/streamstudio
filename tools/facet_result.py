"""Reader for FacetRT / FacetEB results in JSON or binary format (docs/facet-binary-output.md).

    from facet_result import load
    r = load("out/facetrt.json")            # JSON or binary result, same call
    r["sunlit"]                              # numpy float32 array (shape as documented)
    r["meta"]                                # everything that is not a large array
    r = load(path, mmap=True)                # binary arrays as numpy.memmap (not read into memory)

JSON-mode arrays (plain lists) are returned as float32 arrays with the documented shapes so that both
formats give the same result. Run as a script to print a summary: python facet_result.py result.json
"""
import json
import os
import sys

import numpy as np

# Documented shapes of the large arrays (see docs/facet-binary-output.md): n = facets, s = sides = 2n.
_PER_SIDE = ("sunlit", "radiosity", "lightEnhancement", "temperature", "temperatureSunlit", "temperatureShaded",
             "netRadiation", "sensibleHeat", "latentHeat", "storageHeat", "photovoltaicPower")


def _is_descriptor(value):
    return isinstance(value, dict) and "dtype" in value and "file" in value and "offset" in value


def _read_descriptor(directory, desc, mmap):
    path = os.path.join(directory, desc["file"])
    dtype = np.dtype("<" + {"float32": "f4", "int32": "i4", "float64": "f8"}[desc["dtype"]])
    shape = tuple(desc.get("shape") or [desc["length"]])
    if mmap:
        return np.memmap(path, dtype=dtype, mode="r", offset=int(desc["offset"]), shape=shape)
    return np.fromfile(path, dtype=dtype, count=int(desc["length"]), offset=int(desc["offset"])).reshape(shape)


def load(path, mmap=False):
    """Load a facetrt.json / faceteb.json result. Returns a dict: every array field as a numpy array,
    scalar/object fields as they are, plus ``meta`` (the JSON document without the large arrays) and
    ``format`` ("json" or "binary")."""
    path = str(path)
    directory = os.path.dirname(os.path.abspath(path))
    with open(path, "r", encoding="utf-8") as f:
        doc = json.load(f)
    result = {"format": doc.get("format", "json")}
    meta = {}
    for key, value in doc.items():
        if _is_descriptor(value):
            result[key] = _read_descriptor(directory, value, mmap)
        elif isinstance(value, list) and value and isinstance(value[0], (int, float)) and len(value) > 1024:
            result[key] = np.asarray(value, dtype=np.float32)
        else:
            result[key] = value
            meta[key] = value
    # Give JSON-mode arrays the same shapes as binary descriptors.
    n = int(doc.get("facetCount", 0))
    if n:
        s = 2 * n
        if "vertexPositions" in result and result["vertexPositions"].ndim == 1:
            result["vertexPositions"] = result["vertexPositions"].reshape(n, 3, 3)
        bands = max(len(doc.get("wavelengths", [])), 1)
        if "bandRadiosity" in result and result["bandRadiosity"].ndim == 1:
            result["bandRadiosity"] = result["bandRadiosity"].reshape(bands, s)
        if "bandStates" in result and result["bandStates"].ndim == 1:
            result["bandStates"] = result["bandStates"].reshape(bands, s, 2)
    result["meta"] = meta
    return result


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(64)
    data = load(sys.argv[1])
    print("format:", data["format"])
    for key, value in data.items():
        if isinstance(value, np.ndarray):
            print(f"  {key}: {value.dtype} {tuple(value.shape)}")
