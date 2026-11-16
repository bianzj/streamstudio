"""H.264-capable encoder selection for the scene-animation video products.

ffmpeg builds differ: the conda ffmpeg 8 ships libopenh264 but no libx264. Long renders
used to fail only when the first frame reached the encoder. ``pick_encoder`` runs once at
start-up, before any rendering: it lists the encoders and test-encodes two tiny frames with
the chosen one. Preference: libx264 -> libopenh264 -> mpeg4; ``--video-encoder`` overrides.
Standard library only.
"""
from __future__ import annotations

import subprocess
from pathlib import Path

PREFERENCE = ('libx264', 'libopenh264', 'mpeg4')
CHOICES = ('auto',) + PREFERENCE


class EncoderError(RuntimeError):
    """No usable video encoder in the given ffmpeg."""


def listed_encoders(ffmpeg) -> set[str]:
    r = subprocess.run([str(ffmpeg), '-hide_banner', '-encoders'], capture_output=True)
    if r.returncode != 0:
        raise EncoderError(f'cannot run {ffmpeg} -encoders: ' + r.stderr.decode(errors='replace')[:400])
    names = set()
    for line in r.stdout.decode(errors='replace').splitlines():
        parts = line.split()
        if len(parts) >= 2 and len(parts[0]) == 6 and parts[0][0] in 'VAS':
            names.add(parts[1])
    return names


def _works(ffmpeg, name) -> tuple[bool, str]:
    # Raw RGB frames on stdin: the same input path the renderers use (some builds lack lavfi).
    frames = bytes(64 * 64 * 3) * 2
    r = subprocess.run([str(ffmpeg), '-v', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', '64x64', '-r', '24', '-i', '-',
                        '-frames:v', '2', '-pix_fmt', 'yuv420p', '-c:v', name, '-f', 'null', '-'], input=frames, capture_output=True)
    return r.returncode == 0, r.stderr.decode(errors='replace')[:400]


def pick_encoder(ffmpeg, override: str | None = None) -> str:
    """Return the encoder name to use; raise EncoderError before any rendering if none works."""
    ffmpeg = Path(ffmpeg)
    listed = listed_encoders(ffmpeg)
    candidates = PREFERENCE if override in (None, '', 'auto') else (override,)
    if override not in (None, '', 'auto') and override not in PREFERENCE:
        raise EncoderError(f'unsupported --video-encoder {override!r}; choose one of {", ".join(CHOICES)}')
    problems = []
    for name in candidates:
        if name not in listed:
            problems.append(f'{name}: not in this ffmpeg build')
            continue
        ok, error = _works(ffmpeg, name)
        if ok:
            return name
        problems.append(f'{name}: test encode failed ({error.strip()})')
    raise EncoderError(f'no usable H.264/MPEG-4 encoder in {ffmpeg}: ' + '; '.join(problems) +
                       '. Install an ffmpeg with libx264 (or libopenh264) or pass --ffmpeg to another build.')


def encoder_args(name: str, crf=18, preset: str | None = None) -> list[str]:
    """Codec arguments for ``name`` at roughly the quality of ``-crf crf`` with libx264."""
    if name == 'libx264':
        return ['-c:v', 'libx264'] + (['-preset', str(preset)] if preset else []) + ['-crf', str(crf)]
    if name == 'libopenh264':
        # libopenh264 has no CRF; a fixed high bitrate keeps the generic visualisation quality.
        return ['-c:v', 'libopenh264', '-b:v', '8M']
    if name == 'mpeg4':
        return ['-c:v', 'mpeg4', '-q:v', '2']
    raise EncoderError('unknown encoder ' + repr(name))


def receipt(name: str, ffmpeg) -> dict:
    return dict(encoder=name, ffmpeg=str(ffmpeg), selection='--video-encoder override or first working of ' + ' > '.join(PREFERENCE))


def add_argument(parser) -> None:
    parser.add_argument('--video-encoder', choices=CHOICES, default='auto',
                        help='H.264 encoder; auto = first working of libx264, libopenh264, mpeg4 (probed before rendering)')
