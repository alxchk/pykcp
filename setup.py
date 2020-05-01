from distutils.core import setup, Extension
import platform

system = platform.system()
if system == 'Windows':
    cflags = []
    libs = [ 'ws2_32' ]

else:
    cflags = [ '-g', '-O3', '-Wall', '-std=gnu89' ]
    if system == 'Linux':
        libs = [ 'rt' ]
    else:
        libs = []

kcp = Extension(
    'kcp',
    sources=[ 'pykcp.c' ],
    libraries=libs,
    extra_compile_args=cflags
)

setup(
    name = 'KCP',
    version = '1.0.3',
    description = 'Python KCP Bindings',
    ext_modules = [kcp]
)
