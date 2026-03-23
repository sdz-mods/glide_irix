# 3dfx glide IRIX port

GLIDE port for IRIX.

At the moment, glide2x is ported to IRIX, with support for SST1 (Voodoo1) on SGI IP32 (O2).
All glide2x sst1 tests run and render correctly.

CVG (Voodoo2) / IP30 or other systems not yet supported.

## Building

### Prerequisites

Install the complete IRIX development environment:

- MIPSPro 7.4.4 compiler (install 7.4, then patch to 7.4.4m)
- Development Foundation 1.3
- Development Libraries February 2002 (latest version)

Build and install the IRIX 3dfx kernel driver:
- https://github.com/sdz-mods/tdfx_irix


### Build and Install

```csh
#clone or copy this repo onto the target system, e.g. /usr/3dfx_irix/glide_irix
cd /usr/3dfx_irix/glide_irix/swlibs/newpci/pcilib
smake -f Makefile.pcilib clean && smake -f Makefile.pcilib

cd /usr/3dfx_irix/glide_irix/glide2x/sst1/init
smake -f Makefile.sst1init clean && smake -f Makefile.sst1init

cd /usr/3dfx_irix/glide_irix/glide2x/sst1/glide/src
smake -f Makefile.glide_irix clean && smake -f Makefile.glide_irix && smake -f Makefile.glide_irix install
```

### Build glide2x sst1 tests

```csh
cd /usr/3dfx_irix/glide_irix/glide2x/sst1/glide/tests
smake -f Makefile.tests_irix
```


## Tested with

- IP32, RM7000C CPU, Irix 6.5.30, Voodoo1


## License

Source code is licensed under the 3DFX GLIDE Source Code General Public License.
