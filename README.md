# regexfs 
A FUSE filesystem for changing filenames via regex

## 1. Introduction

regexfs is a FUSE filesystem that allows the user to mirror a local directory
to another, somewhat like using `mount --bind`. In that process, it is able to
change the existing filenames via a regex, thus allowing dynamic renaming.


## 2. Setup

### 2.1. Compilation

Firstly you need the FUSE library and headers, these can be installed via your
package manager. Afterwards, a simple call to `make' is normally all you will 
need to compile regexfs:

```bash
$ make
```

You can override the CFLAGS variable to provide optimisation flags e.t.c. The
DEBUG variable can be used to enable debugging - if you are using gcc, setting
DEBUG to "-g" would probably do.


### 2.2. Installation

First verify with `make -n install` the installation paths. By default, the
installation prefix is `/usr/local` and the fuseflect binary is installed in
`$(prefix)/bin`. You can set the prefix and bindir variables to override these
defaults. Then run `make install` as root to perform the actual installation:

```bash
# make install
```

### 2.3. Running

Please read the FUSE documentation before using regexfs.

Run `regexfs -h` to see the various command line options. All you have to do
in most cases is:

```bash
$ regexfs source_directory/ -o 'regex=regex1:replace1;replace2;regex2:replace3;...' mountpoint/
```

If you want to use regexfs from /etc/fstab, the following example shows its
use:

regexfs#/usr/share/doc /fs/vfs/doc fuse defaults,regex=... 0 0

If you do use regexfs in your /etc/fstab, you will probably want to add the
allow_other and default_permissions options. The first option allows non-root
users to access regexfs, while the second one activates the in-kernel
permission checking, since regexfs does no access control on its own.

Please note that you may need to replace whitespace characters in the regex
and escape others to allow proper parsing of the file.

WARNING: _NEVER_ USE allow_other WITHOUT default_permissions, OR YOU RISK
ALLOWING UNPRIVILEDGED PROCESSES TO ACCESS FILES THEY SHOULD NOT BE ABLE TO.

### 3. License

Regexfs is released under the GNU General Public License version 2. The full
text of the license can be found in the COPYING file that should be included in
the regexfs distribution.

### 4. Authors

Regexfs author:
    Jendrik Weise <jewe37@gmail.com>

Original author:
	Theodoros V. Kalamatianos <nyb@users.sourceforge.net>
