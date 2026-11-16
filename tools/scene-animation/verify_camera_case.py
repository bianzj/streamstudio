"""Verify case provenance, frozen artifacts and canonical-channel isolation."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

from prepare_thermal_case import require, sha


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def main():
    p = argparse.ArgumentParser()
    p.add_argument('case', type=Path)
    p.add_argument('reference', type=Path)
    p.add_argument('repo', type=Path)
    a = p.parse_args()
    case, previous, repo = a.case.resolve(), a.reference.resolve(), a.repo.resolve()
    run, reference = case / 'result_final', previous / 'result_final'
    m, old = read(run / 'run-manifest.json'), read(reference / 'run-manifest.json')
    require(m['status'] == 'complete', 'Native render incomplete')
    require(m['timeline'] == old['timeline'], 'Physics timeline/diagnostics changed')
    same = {}
    for folder in ['frames', 'fixed_frames', 'radiance', 'geometry']:
        left, right = sorted((reference / folder).glob('*')), sorted((run / folder).glob('*'))
        require([f.name for f in left] == [f.name for f in right], 'Canonical file coverage changed')
        for f, g in zip(left, right):
            require(sha(f) == sha(g), 'Canonical image/radiance changed: ' + str(g))
        same[folder] = len(left)
    for name in ['rigid.csv', 'transport.csv']:
        require(sha(run / name) == sha(reference / name), 'Physical diagnostics changed: ' + name)
    guard_fix_equal = 0
    before_guard = case / 'result_final_before_boundary_fix'
    if before_guard.exists():
        for folder in ['optical_frames', 'observed_ir', 'observed_optical', 'detector_ir']:
            for f in (run / folder).glob('*.png'):
                require(sha(f) == sha(before_guard / folder / f.name), 'Input-boundary correction changed imagery')
                guard_fix_equal += 1
    protected = read(case / 'protected.json')
    for path, digest in protected.items():
        require(sha(repo / path) == digest, 'Protected repository file changed: ' + path)
    frozen_source = read(case / 'frozen-source.json')
    for path, digest in frozen_source.items():
        require(sha(previous / 'source' / path) == digest, 'Prior source snapshot changed: ' + path)
    frozen_output = read(case / 'frozen-output.json')
    for path, digest in frozen_output.items():
        require(sha(previous / path) == digest, 'Prior packaged artifact changed: ' + path)
    frozen_frames = read(case / 'frozen-native-frames.json')['nativeFrameSha256']
    for path, digest in frozen_frames.items():
        require(sha(reference / path) == digest, 'Prior native frame changed: ' + path)
    sources = read(case / 'source-sha256.json')
    for path, digest in sources.items():
        require(sha(repo / path) == digest and sha(case / 'source' / path) == digest,
                'Current source/snapshot mismatch: ' + path)
    require(sha(run / 'provenance/histream.exe') == sha(repo / 'build/histream-animation/runtime/Release/histream.exe'), 'Engine binary changed after rendering')
    for shader in (run / 'provenance/camera').glob('*.spv'):
        require(sha(shader) == sha(repo / 'build/histream-animation/runtime/Release/shader/animation/camera' / shader.name), 'Camera shader version mismatch')
    video = read(case / 'video/verification.json')
    require(video['status'] == 'passed' and all(v['decodedFrames'] == m['frameCount'] for v in video['videos'].values()), 'Video verification incomplete')
    fixture = read(case / 'fixtures_final/verification.json')
    require(fixture['status'] == 'passed', 'Numerical camera fixtures incomplete')
    require(read(case / 'test-verification.json')['status'] == 'passed', 'Integration test receipt incomplete')
    onboard = case / 'onboard_ir'
    pose, onboard_manifest = read(onboard / 'pose.json'), read(onboard / 'result_final/run-manifest.json')
    require(onboard_manifest['status'] == 'complete' and onboard_manifest['frameCount'] == 1, 'Onboard still incomplete')
    frame = int(pose['frame'])
    native_image = onboard / f'result_final/observed_ir/frame_{frame:04d}.png'
    require(sha(onboard / 'onboard_ir.png') == sha(native_image), 'Onboard delivered image differs from native render')
    adc = onboard / f'result_final/detector_ir/frame_{frame:04d}.png'
    with Image.open(adc) as im:
        values = np.asarray(im)
        require(im.size == tuple(m['cameraObservation']['ir']['pixels']) and values.max() <= 16383,
                'Onboard ADC dimensions/range invalid')
    require(adc.read_bytes()[24] == 16, 'Native ADC must use a 16-bit PNG container')
    require(pose['timeSeconds'] < m['timeline']['firstContact']['timeSeconds'], 'Onboard still time outside approach')
    receipt = dict(status='passed', canonicalIdenticalFiles=same, timelineExact=True, rigidCsvExact=True,
                   transportCsvExact=True, previousSourceFilesVerified=len(frozen_source),
                   previousOutputsVerified=len(frozen_output), previousNativeFramesVerified=len(frozen_frames),
                   protectedRepositoryFilesVerified=len(protected), currentSourceSnapshotFiles=len(sources),
                   boundaryCorrectionIdenticalImageFiles=guard_fix_equal,
                   numericalCameraFixture=fixture, device=m['device'], wallSeconds=m['wallSeconds'],
                   decodedVideos=video['videos'], onboardStill=dict(path=str(onboard / 'onboard_ir.png'), sha256=sha(native_image),
                       timeSeconds=pose['timeSeconds'], detectorPixels=list(im.size)),
                   calibratedDeviceValidation=False, structuralResponseComputed=False)
    (case / 'final-verification.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main()
