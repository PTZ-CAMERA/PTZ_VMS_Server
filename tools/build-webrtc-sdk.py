"""고정 libdatachannel 소스에서 VMS 내부 WebRTC SDK를 빌드한다. 외부 서버 실행 없음."""
import argparse
from pathlib import Path
import subprocess

PIN='6b1e2e620f1e37f0eafeee702eaea0043cb305fd'  # v0.24.6
ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--jobs',type=int,default=3)
    args=parser.parse_args()
    root=ROOT/'build/dependencies';source=root/'libdatachannel';build=root/'libdatachannel-build';prefix=root/'webrtc-sdk'
    root.mkdir(parents=True,exist_ok=True)
    if not source.exists():subprocess.run(['git','clone','--branch','v0.24.6','--depth','1','--recurse-submodules','--shallow-submodules','https://github.com/paullouisageneau/libdatachannel.git',str(source)],check=True)
    head=subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
    if head!=PIN:raise RuntimeError('Unexpected libdatachannel revision')
    subprocess.run(['cmake','-S',str(source),'-B',str(build),'-DCMAKE_BUILD_TYPE=Release',f'-DCMAKE_INSTALL_PREFIX={prefix}',
        '-DBUILD_SHARED_LIBS=OFF','-DNO_EXAMPLES=ON','-DNO_TESTS=ON','-DNO_WEBSOCKET=ON','-DUSE_SYSTEM_JSON=ON'],check=True)
    subprocess.run(['cmake','--build',str(build),'--config','Release','--parallel',str(args.jobs)],check=True)
    subprocess.run(['cmake','--install',str(build),'--config','Release'],check=True)
    print(f'Configure VMS with -DCMAKE_PREFIX_PATH={prefix}')
if __name__=='__main__':main()
