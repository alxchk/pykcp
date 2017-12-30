from distutils.core import setup, Extension

kcp = Extension(
    'kcp',
    sources = ['pykcp.c'],
    libraries = ['rt'],
)

setup(
    name = 'KCP',
    version = '1.0',
    description = 'Python KCP Bindings',
    ext_modules = [kcp]
)
