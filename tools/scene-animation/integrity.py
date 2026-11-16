"""Integrity/acceptance checks that stay active under ``python -O``.

``assert`` statements are stripped when Python runs with ``-O`` or ``PYTHONOPTIMIZE``;
production input, resume, seal, acceptance and video checks therefore use ``require``.
Standard library only.
"""
from __future__ import annotations


class IntegrityError(RuntimeError):
    """An input, resume, seal, acceptance or product check failed."""


def require(condition, message='integrity check failed'):
    """Raise IntegrityError unless ``condition`` is truthy.

    ``message`` may be a zero-argument callable; it is evaluated only on failure, so
    expensive or failure-only diagnostics (decoded ffmpeg stderr, large tuples) cost nothing
    on the success path.
    """
    if condition:
        return
    if callable(message):
        message = message()
    if not isinstance(message, str):
        message = repr(message)
    raise IntegrityError(message)
