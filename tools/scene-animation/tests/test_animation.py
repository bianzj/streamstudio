"""Targeted native CLI/GPU integration checks; requires a built histream.exe."""
import argparse
import json
import subprocess
import os
from pathlib import Path

from PIL import Image, ImageChops


def main():
    p = argparse.ArgumentParser()
    p.add_argument('engine', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    config = a.output / 'input.json'
    def run(data, directory, bare=False, relative=False):
        config.write_text(json.dumps(data), encoding='utf-8')
        env = dict(os.environ, PATH=str(a.engine.resolve().parent)+os.pathsep+os.environ.get('PATH',''))
        output_arg = directory.name + os.sep if relative else str(directory.resolve())
        result = subprocess.run([a.engine.name if bare else str(a.engine.resolve()), 'eSceneAnimation', str(config.resolve()), output_arg], executable=str(a.engine.resolve()), capture_output=True, cwd=a.output.resolve(), env=env)
        text = (result.stdout + result.stderr).decode('utf-8', errors='replace')
        (a.output / (directory.name + '.log')).write_text(text, encoding='utf-8')
        return result.returncode, text
    for data, label, expected in [
        ({'width':0}, 'invalid_width', 'width'),
        ({'fps':0}, 'invalid_fps', 'fps'),
        ({'frameCount':7,'firstFrame':8}, 'invalid_range', 'frame'),
        ({'impactTime':-1}, 'invalid_time', 'impactTime'),
        ({'width':4294967328,'frameCount':1}, 'overflow_width', 'width'),
        ({'frameCount':4294967297}, 'overflow_count', 'frameCount'),
        ({'renderMode':'unknown','width':32,'height':32,'frameCount':1}, 'invalid_mode', 'renderMode'),
        ({'thermalDisplayMinK':700,'thermalDisplayMaxK':300,'width':32,'height':32,'frameCount':1}, 'invalid_thermal_range', 'thermalDisplay'),
        ({'thermalBandMode':'unknown','width':32,'height':32,'frameCount':1}, 'invalid_band', 'thermalBandMode'),
    ]:
        code, text = run(data, a.output / label)
        assert code != 0 and expected.lower() in text.lower(), (label, code, text)
    data = {'width':192,'height':108,'fps':1,'frameCount':7,'impactTime':3,'samples':1}
    dest = a.output / 'gpu_sequence'
    code, text = run(data, dest)
    assert code == 0, text
    manifest = json.loads((dest / 'run-manifest.json').read_text(encoding='utf-8'))
    assert manifest['backend'] == 'gpu' and manifest['status'] == 'complete'
    assert manifest['frameCount'] == 7 and manifest['physicsValidated'] is False
    frames = sorted((dest / 'frames').glob('frame_*.png'))
    assert len(frames) == 7
    pictures = []
    for frame in frames:
        with Image.open(frame) as im:
            assert im.size == (192,108)
            assert max(im.convert('RGB').getextrema()[0]) > 32
            pictures.append(im.convert('RGB').copy())
    assert ImageChops.difference(pictures[0],pictures[2]).getbbox(), 'projectile must move'
    assert ImageChops.difference(pictures[2],pictures[4]).getbbox(), 'impact must change scene'
    assert ImageChops.difference(pictures[4],pictures[6]).getbbox(), 'smoke must evolve'
    original = frames[0].read_bytes()
    code, text = run(data, dest)
    assert code != 0 and 'exist' in text.lower(), text
    assert frames[0].read_bytes() == original, 'existing results must be preserved'
    # A frame rendered alone must match its frame in a full sequence.
    partial = a.output / 'gpu_single'
    code, text = run(dict(data, firstFrame=5, lastFrame=5), partial, bare=True, relative=True)
    assert code == 0, text
    assert (partial/'frames'/'frame_0005.png').read_bytes() == frames[4].read_bytes()
    assert (partial/'provenance'/'renderer.comp.spv').is_file()
    assert (partial/'provenance'/'histream.exe').is_file()
    thermal = a.output / 'thermal_sequence'
    thermal_data = dict(data, renderMode='thermal')
    code, text = run(thermal_data, thermal)
    assert code == 0, text
    tm = json.loads((thermal/'run-manifest.json').read_text(encoding='utf-8'))
    assert tm['renderMode']=='thermal' and tm['thermalFieldIsPrescribed'] is True
    ir = []
    for path in sorted((thermal/'frames').glob('frame_*.png')):
        with Image.open(path) as image:
            channels = image.convert('RGB').split()
            assert ImageChops.difference(channels[0],channels[1]).getbbox() is None
            assert ImageChops.difference(channels[1],channels[2]).getbbox() is None
            ir.append(image.convert('RGB').copy())
    assert len(ir)==7
    roi=(int(192*.4),int(108*.4),int(192*.58),int(108*.7))
    assert ir[4].crop(roi).getextrema()[0][1] > ir[2].crop(roi).getextrema()[0][1]+20, 'hot region must brighten after impact'
    alternate = a.output/'thermal_other_wavelength'
    code,text = run(dict(thermal_data,firstFrame=5,lastFrame=5,thermalWavelengthMicrometers=8),alternate)
    assert code==0,text
    with Image.open(alternate/'frames'/'frame_0005.png') as other:
        assert ImageChops.difference(ir[4],other.convert('RGB')).getbbox(), 'wavelength must affect thermal rendering'
    band=a.output/'thermal_band'
    code,text=run(dict(thermal_data,firstFrame=5,lastFrame=5,thermalBandMode='lwir8_14'),band)
    assert code==0,text
    bm=json.loads((band/'run-manifest.json').read_text(encoding='utf-8'))
    assert bm['thermal']['bandMinMicrometers']==8 and bm['thermal']['bandMaxMicrometers']==14
    with Image.open(band/'frames'/'frame_0005.png') as other:
        assert ImageChops.difference(ir[4],other.convert('RGB')).getbbox(), 'band-integrated radiance must affect display'
    print('PASS: input boundaries, GPU execution, output protection, deterministic seeking, thermal grayscale, hot-region contrast and wavelength response')


if __name__ == '__main__':
    main()
