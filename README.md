regexfs - A FUSE filesystem for changing filenames via regex



Contents:

    1. Introduction
    2. Platforms
    3. Setup
	3.1. Compilation
	3.2. Installation
	3.3. Running
    4. License
    5. Authors


1. Introduction

regexfs is a FUSE filesystem that allows the user to mirror a local directory
to another, somewhat like using `mount --bind'. In that process, it is able to
change the existing filenames via a regex, thus allowing dynamic renaming.


2. Setup

2.1. Compilation

Firstly you need the FUSE library and headers, these can be installed via your
package manager. Afterwards, a simple call to `make' is normally all you will 
need to compile regexfs:

```bash
$ make
```

You can override the CFLAGS variable to provide optimisation flags e.t.c. The
DEBUG variable can be used to enable debugging - if you are using gcc, setting
DEBUG to "-g" would probably do.


3.2. Installation

First verify with `make -n install' the installation paths. By default, the
installation prefix is /usr/local and the fuseflect binary is installed in
$(prefix)/bin. You can set the prefix and bindir variables to override these
defaults. Then run `make install' as root to perform the actual installation:

```bash
# make install
```


3.3. Running

Please read the FUSE documentation before using regexfs.

Run `regexfs -h' to see the various command line options. All you have to do
in most cases is:

$ regexfs source_directory/ 'regex1:replace1;replace2,regex2:replace3,...' mountpoint/

WARNING: _NEVER_ USE allow_other WITHOUT default_permissions, OR YOU RISK
ALLOWING UNPRIVILEDGED PROCESSES TO ACCESS FILES THEY SHOULD NOT BE ABLE TO.

4. License

Regexfs is released under the GNU General Public License version 2. The full
text of the license can be found in the COPYING file that should be included in
the regexfs distribution.

5. Authors

Regexfs author:
    Jendrik Weise <jewe37@gmail.com>

Original author:
	Theodoros V. Kalamatianos <nyb@users.sourceforge.net>
