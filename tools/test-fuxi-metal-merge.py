#!/usr/bin/env python3
"""Production render-pass merge A/B pixels and 140-draw host benchmark; no downloads."""
import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--headers', type=Path, default=Path('/usr/include'))
    ap.add_argument('--icd', type=Path, default=Path('/lib/x86_64-linux-gnu/libvulkan_lvp.so'))
    ap.add_argument('--glslang', default=shutil.which('glslangValidator'))
    a = ap.parse_args()
    if not a.glslang:
        ap.error('supply an existing glslangValidator with --glslang')
    spec = importlib.util.spec_from_file_location('m6', ROOT/'tools/test-fuxi-metal-m6.py')
    m6 = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m6)
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', LP_NUM_THREADS='2', CONTROL_LOD='0')
        common = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I'+str(a.headers)]
        sanitized = ['-O1', '-fsanitize=address,undefined']
        fragment = '''#version 450
layout(set=0,binding=0) uniform sampler2D image;
layout(push_constant) uniform Constants { float lod; } constants;
layout(location=0) out vec4 color;
void main() {color=textureLod(image,vec2(0.5),constants.lod);}
'''
        storage = fragment.replace('layout(location=0)', 'layout(set=0,binding=1,std430) buffer State {uint value;} state;\nlayout(location=0)').replace('color=textureLod(image,vec2(0.5),constants.lod);', 'state.value+=1; color=vec4(float(state.value)/255.0,0,0,1);')
        for stage, source in [('vert', m6.VERTEX), ('frag', fragment), ('storage.frag', storage)]:
            src = d/('control.'+stage)
            src.write_text(source)
            out = d/(stage+'.spv')
            subprocess.run([a.glslang, '-V', '--target-env', 'vulkan1.2', str(src), '-o', str(out)], check=True, capture_output=True)
            env[{'vert': 'CONTROL_VERTEX', 'frag': 'CONTROL_FRAGMENT', 'storage.frag': 'CONTROL_FRAGMENT_STORAGE'}[stage]] = str(out)
        subprocess.run(common+['-shared', '-fPIC', str(ROOT/'tools/fuxi-metal/RenderMergeProxy.cpp'), '-ldl', '-o', str(d/'proxy.so')], check=True)
        env['SOFTWARE_ICD'] = str(a.icd.resolve())
        definitions = ['-DFUXI_MTL_HOST_TEST', '-DFUXI_MTL_ICD="'+str(d/'proxy.so')+'"', '-DFUXI_MTL_TRANSLATOR="'+str(d/'bridge.so')+'"']
        subprocess.run(common+sanitized+['-shared', '-fPIC', str(ROOT/'tools/fuxi-metal/RenderMergeBridge.cpp'), '-o', str(d/'bridge.so')], check=True)
        subprocess.run(common+sanitized+definitions+[str(ROOT/'tools/fuxi-metal/RenderMergeTests.cpp'), '-ldl', '-pthread', '-o', str(d/'test')], check=True)
        def invoke(binary, args):
            p = subprocess.run([str(binary)], env=dict(env, FUXI_MTL_TEST_BOOTARGS='fuxigpuframetrace=1 '+args), text=True, capture_output=True, timeout=90)
            assert p.returncode == 0, p.stderr
            assert 'AddressSanitizer' not in p.stderr and 'runtime error:' not in p.stderr, p.stderr
            return p.stdout
        for extra in ('', 'fuxigpufeedback=1', 'fuxigpuoverlap=1 fuxigpusubmit=1', 'fuxigpuoverlap=1 fuxigpusubmit=1 fuxigpufeedback=1'):
            baseline = invoke(d/'test', extra)
            merged = invoke(d/'test', extra+' fuxigpumerge=1')
            assert baseline == merged, 'merged pixels differ: '+extra
            print('PASS: byte-identical A/B render merge controls '+extra)
        for bad in ('0', 'bad', '1 fuxigpumerge=1'):
            invoke(d/'test', 'fuxigpumerge='+bad)
        print('PASS: disabled/malformed/duplicate boot tokens preserve default pass counts')
        # Benchmark with the existing nonblending adapter, so the final pixel is
        # also an independently known value. Fresh-process A/B/B/A, no threshold.
        subprocess.run(common+['-O2', '-shared', '-fPIC', str(ROOT/'tools/fuxi-metal/MipFeedbackBridge.cpp'), '-o', str(d/'bench-bridge.so')], check=True)
        definitions[1] = '-DFUXI_MTL_ICD="'+str(a.icd.resolve())+'"'
        definitions[-1] = '-DFUXI_MTL_TRANSLATOR="'+str(d/'bench-bridge.so')+'"'
        subprocess.run(common+['-O2']+definitions+[str(ROOT/'tools/fuxi-metal/SubmitBench.cpp'), '-ldl', '-pthread', '-o', str(d/'bench')], check=True)
        for fast in ('', 'fuxigpusubmit=1'):
            for args in ('', 'fuxigpumerge=1', 'fuxigpumerge=1', ''):
                print(invoke(d/'bench', fast+' '+args), end='')

if __name__ == '__main__':
    main()
