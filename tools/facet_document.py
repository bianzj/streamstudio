"""Read a FacetRT / FacetEB result *document* (facetrt.json / faceteb.json) in JSON or binary format.

    from facet_document import read_document
    doc = read_document("out/faceteb.json")          # dict, same keys as json.load()
    vertices = np.asarray(doc["vertexPositions"], dtype=float).reshape(-1, 3, 3)

For JSON-format files this is exactly ``json.load`` (values stay plain Python lists, so existing code that does
``np.asarray(doc[key], dtype=...)`` behaves bit-for-bit as before). For binary-format files (docs/facet-binary-output.md,
``"format": "binary"``) every array descriptor ``{file, offset, length, dtype, shape}`` is replaced by a numpy array of the
documented shape read from the sidecar .bin (via facet_result.py), so the same ``np.asarray(...)`` calls work unchanged.
``only`` restricts which descriptor fields are read (others are left as the descriptor dict) to avoid loading
multi-GB arrays that a script does not use. See facet_result.load for the full-featured loader.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from facet_result import _is_descriptor, _read_descriptor  # noqa: E402  (shared descriptor reader)


def read_document(path, only=None, mmap=False):
    path = str(path)
    with open(path, "r", encoding="utf-8-sig") as stream:
        doc = json.load(stream)
    if not isinstance(doc, dict) or doc.get("format") != "binary":
        return doc
    directory = os.path.dirname(os.path.abspath(path))
    for key, value in list(doc.items()):
        if _is_descriptor(value) and (only is None or key in only):
            doc[key] = _read_descriptor(directory, value, mmap)
    return doc
