#!/usr/bin/perl -w

# STB Linux buildroot build system v1.0
# Copyright (c) 2017 Broadcom
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

use strict;
use warnings;
use File::Basename;
use File::Path qw(make_path);
use Getopt::Std;
use POSIX;

use constant AUTO_MK => qw(brcmstb.mk);
use constant LOCAL_MK => qw(local.mk);

my %arch_config = (
	'arm64' => {
		'arch_name' => 'aarch64',
		'BR2_aarch64' => 'y',
		'BR2_cortex_a53' => 'y',
		'BR2_LINUX_KERNEL_DEFCONFIG' => 'brcmstb',
	},
	'arm' => {
		'arch_name' => 'arm',
		'BR2_arm' => 'y',
		'BR2_cortex_a15' => 'y',
		'BR2_LINUX_KERNEL_DEFCONFIG' => 'brcmstb',
	},
	'mips' => {
		'arch_name' => 'mips',
		'BR2_mipsel' => 'y',
		'BR2_MIPS_SOFT_FLOAT' => '',
		'BR2_MIPS_FP32_MODE_32' => 'y',
		'BR2_LINUX_KERNEL_DEFCONFIG' => 'bmips_stb',
	},
);

# It doesn't look like we need to set BR2_TOOLCHAIN_EXTERNAL_CUSTOM_PREFIX
# with stbgcc-6.3-x.y, since it has all the required symlinks.
my %toolchain_config = (
	'arm64' => {
#		'BR2_TOOLCHAIN_EXTERNAL_CUSTOM_PREFIX' => '$(ARCH)-linux-gnu'
	},
	'arm' => {
#		'BR2_TOOLCHAIN_EXTERNAL_CUSTOM_PREFIX' => '$(ARCH)-linux-gnueabihf'
	},
	'mips' => {
#		'BR2_TOOLCHAIN_EXTERNAL_CUSTOM_PREFIX' => '$(ARCH)-linux-gnu'
	},
);

my %generic_config = (
	'BR2_LINUX_KERNEL_CUSTOM_REPO_URL' =>
				'git://stbgit.broadcom.com/queue/linux.git',
	'BR2_LINUX_KERNEL_CUSTOM_REPO_VERSION' => 'stb-4.1',
);

sub check_br()
{
	my $readme = 'README';

	# README file must exist
	return -1 if (! -r $readme);

	open(F, $readme);
	$_ = <F>;
	close(F);

	# First line must contain "Buildroot"
	return 0 if (/Buildroot/);

	return -1;
}


# Check for some obvious build artifacts that show us the local Linux source
# tree is not clean.
sub check_linux($)
{
	my ($local_linux) = @_;

	return 0 if (-e "$local_linux/.config");
	return 0 if (-e "$local_linux/vmlinux");
	return 0 if (-e "$local_linux/vmlinux.o");
	return 0 if (-e "$local_linux/vmlinuz");
	return 0 if (-e "$local_linux/System.map");

	return 1;
}

sub get_cores()
{
	my $num_cores;

	$num_cores = `getconf _NPROCESSORS_ONLN 2>/dev/null`;
	# Maybe somebody wants to run this on BSD? :-)
	if ($num_cores eq '') {
		$num_cores = `getconf NPROCESSORS_ONLN 2>/dev/null`;
	}
	# Still no luck, try /proc.
	if ($num_cores eq '') {
		$num_cores = `grep -c -P '^processor\\s+:' /proc/cpuinfo 2>/dev/null`;
	}
	# Can't figure out the number of cores. Assume just 1 core.
	if ($num_cores eq '') {
		$num_cores = 1;
	}
	chomp($num_cores);

	return $num_cores;
}

sub find_toolchain()
{
	my @path = split(/:/, $ENV{'PATH'});

	foreach my $dir (@path) {
		# We don't support anything before stbgcc-6.x at this point.
		if ($dir =~ /stbgcc-[6-9]/) {
			return $dir;
		}
	}
	return undef;
}

sub move_merged_config($$$$)
{
	my ($prg, $arch, $sname, $dname) = @_;
	my $line;

	open(S, $sname) || die("couldn't open $sname");
	open(D, ">$dname") || die("couldn't create $dname");
	print(D "#" x 78, "\n".
		"# This file was automatically generated by $prg.\n".
		"#\n".
		"# Target architecture: ".uc($arch)."\n".
		"#\n".
		"# ".("-- DO NOT EDIT!!! " x 3)."--\n".
		"#\n".
		"# ".strftime("%a %b %e %T %Z %Y", localtime())."\n".
		"#" x 78, "\n\n");
	while ($line = <S>) {
		chomp($line);
		print(D "$line\n");
	}
	close(D);
	close(S);
	unlink($sname);
}

sub write_localmk($$)
{
	my ($prg, $output_dir) = @_;
	my $local_dest = "$output_dir/".LOCAL_MK;
	my @buf;


	if (open(F, $local_dest)) {
		my $auto_mk = AUTO_MK;

		@buf = <F>;
		close(F);
		# Check if we are already including out auto-generated makefile 
		# snipped. Bail if we do.
		foreach my $line (@buf) {
			return if ($line =~ /include .*$auto_mk/);
		}
	}

	# Add header and include directive for our auto-generated makefile.
	open(F, ">$local_dest");
	print(F "#" x 78, "\n".
		"# The following include was added automatically by $prg.\n".
		"# Please do not remove it. Delete ".AUTO_MK." instead, ".
		"if necessary.\n".
		"# You may also add your own make directives underneath.\n".
		"#" x 78, "\n".
		"#\n".
		"-include $output_dir/".AUTO_MK."\n".
		"#\n".
		"# Custom settings start below.\n".
		"#" x 78, "\n\n");

	# Preserve the contents local.mk had before we started modifying it.
	foreach my $line (@buf) {
		chomp($line);
		print(F $line."\n");
	}

	close(F);
}

sub write_brcmstbmk($$$)
{
	my ($prg, $output_dir, $linux_dir) = @_;
	my $auto_dest = "$output_dir/".AUTO_MK;

	open(F, ">$auto_dest");
	print(F "#" x 78, "\n".
		"# Do not edit. Automatically generated by $prg. It may also ".
		"be deleted\n".
		"# without warning by $prg.\n".
		"#" x 78, "\n".
		"#\n".
		"# You may delete this file manually to remove the settings ".
		"below.\n".
		"#\n".
		"#" x 78, "\n\n".
		"LINUX_OVERRIDE_SRCDIR = $linux_dir\n");
	close(F);
}

sub write_config($$$)
{
	my ($config, $fname, $truncate) = @_;

	unlink($fname) if ($truncate);

	open(F, ">>$fname");
	foreach my $key (keys(%$config)) {
		my $val = $config->{$key};

		# Only write keys that start with BR2_ to config file.
		next if ($key !~ /^BR2_/);

		if ($val eq '') {
			print(F "# $key is not set\n");
			next;
		}

		# Numbers and 'y' don't require quotes. Strings do.
		if ($val ne 'y' && $val !~ /^\d+$/) {
			$val = "\"$val\"";
		}

		print(F "$key=$val\n");
	}
	close(F);
}

sub print_usage($)
{
	my ($prg) = @_;

	print(STDERR "usage: $prg [argument(s)] arm|arm64|mips\n".
		"          -3 <path>....path to 32-bit run-time\n".
		"          -b...........launch build after configuring\n".
		"          -c...........clean (remove output/\$platform)\n".
		"          -D...........use platform's default kernel config\n".
		"          -d <fname>...use <fname> as kernel defconfig\n".
		"          -f <fname>...use <fname> as BR fragment file\n".
		"          -i...........like -b, but also build FS images\n".
		"          -j <jobs>....run <jobs> parallel build jobs\n".
		"          -L <path>....use local <path> as Linux kernel\n".
		"          -l <url>.....use <url> as the Linux kernel repo\n".
		"          -o <path>....use <path> as the BR output directory\n".
		"          -t <path>....use <path> as toolchain directory\n".
		"          -v <tag>.....use <tag> as Linux version tag\n");
}

########################################
# MAIN
########################################
my $prg = basename($0);

my $merged_config = 'brcmstb_merged_defconfig';
my $br_output_default = 'output';
my $temp_config = 'temp_config';
my $is_64bit = 0;
my $relative_outputdir;
my $br_outputdir;
my $local_linux;
my $toolchain;
my $arch;
my %opts;

getopts('3:bcDd:f:ij:L:l:o:t:v:', \%opts);
$arch = $ARGV[0];
# Treat bmips as an alias for mips.
$arch = 'mips' if ($arch eq 'bmips');

$is_64bit = ($arch =~ /64/) if (defined($arch));

if ($#ARGV < 0) {
	print_usage($prg);
	exit(1);
}

if (check_br() < 0) {
	print(STDERR
		"$prg: must be called from buildroot top level directory\n");
	exit(1);
}

if (!defined($arch_config{$arch})) {
	print(STDERR "$prg: unknown architecture $arch\n");
	exit(1);
}

if (defined($opts{'L'}) && defined($opts{'l'})) {
	print(STDERR "$prg: options -L and -l cannot be specified together\n");
	exit(1);
}

# Set local Linux directory from environment, if configured.
if (defined($ENV{'BR_LINUX_OVERRIDE'})) {
	$local_linux = $ENV{'BR_LINUX_OVERRIDE'};
}

# Command line option -L supersedes environment to specify local Linux directory
if (defined($opts{'L'})) {
	# Option "-L -" clears the local Linux directory. This can be used to
	# pretend environment variable BR_LINUX_OVERRIDE is not set, without 
	# having to clear it.
	if ($opts{'L'} eq '-') {
		undef($local_linux);
	} else {
		$local_linux = $opts{'L'};
	}
}

if (defined($local_linux) && !check_linux($local_linux)) {
	print(STDERR "$prg: your local Linux directory must be pristine; ".
		"pre-existing\n".
		"configuration files or build artifacts can interfere with ".
		"the build.\n");
	exit(1);
}

if (defined($opts{'o'})) {
	print("Using ".$opts{'o'}." as output directory...\n");
	$br_outputdir = $opts{'o'};
	$relative_outputdir = $br_outputdir;
} else {
	# Output goes under ./output/ by default. We use an absolute path.
	$br_outputdir = getcwd()."/$br_output_default";
	$relative_outputdir = $br_output_default;
}
# Always add arch-specific sub-directory to output directory.
$br_outputdir .= "/$arch";
$relative_outputdir .= "/$arch";

# Create output directory. "make defconfig" needs it to store $temp_config
# before it would create it itself.
if (! -d $br_outputdir) {
	make_path($br_outputdir);
}

# Our temporary defconfig goes in the output directory.
$temp_config = "$br_outputdir/$temp_config";

if (defined($opts{'c'})) {
	my $status;

	print("Cleaning $br_outputdir...\n");
	$status = system("rm -rf \"$br_outputdir\"");
	$status = ($status >> 8) & 0xff;
	exit($status);
}

$toolchain = find_toolchain();
if (!defined($toolchain) && !defined($opts{'t'})) {
	print(STDERR
		"$prg: couldn't find toolchain in your path, use option -t\n");
	exit(1);
}

if (defined($opts{'D'})) {
	print("Using default Linux kernel configuration...\n");
	$arch_config{$arch}{'BR2_LINUX_KERNEL_USE_ARCH_DEFAULT_CONFIG'} = 'y';
	delete($arch_config{$arch}{'BR2_LINUX_KERNEL_DEFCONFIG'});
}

if (defined($opts{'d'})) {
	my $cfg = $opts{'d'};

	# Make it nice for the user and strip trailing _defconfig.
	$cfg =~ s/_?defconfig$//;
	print("Using $cfg as Linux kernel configuration...\n");
	$arch_config{$arch}{'BR2_LINUX_KERNEL_DEFCONFIG'} = $cfg;
}

if (defined($opts{'j'})) {
	my $jval = $opts{'j'};

	if ($jval !~ /^\d+$/) {
		print(STDERR "$prg: option -j requires an interger argument\n");
		exit(1);
	}
	if ($jval < 1) {
		print(STDERR "$prg: the argument to -j must be 1 or larger\n");
		exit(1);
	}

	if ($jval == 1) {
		print("Disabling parallel builds...\n");
	} else {
		print("Configuring for $jval parallel build jobs...\n");
	}
	$generic_config{'BR2_JLEVEL'} = $jval;
} else {
	$generic_config{'BR2_JLEVEL'} = get_cores() + 1;
}

if (defined($local_linux)) {
	print("Using $local_linux as Linux kernel directory...\n");
	write_brcmstbmk($prg, $relative_outputdir, $local_linux);
	write_localmk($prg, $relative_outputdir);
} else {
	# Delete our custom makefile, so we don't override the Linux directory.
	if (-e "$br_outputdir/".AUTO_MK) {
		unlink("$br_outputdir/".AUTO_MK);
	}
}

if (defined($opts{'l'})) {
	print("Using ".$opts{'l'}." as Linux kernel repo...\n");
	$generic_config{'BR2_LINUX_KERNEL_CUSTOM_REPO_URL'} = $opts{'l'};
}

if (defined($opts{'t'})) {
	$toolchain = $opts{'t'};
	print("Using $toolchain as toolchain...\n");
	$toolchain_config{$arch}{'BR2_TOOLCHAIN_EXTERNAL_PATH'} = $toolchain;
}

if (defined($opts{'v'})) {
	print("Using ".$opts{'v'}." as Linux kernel version...\n");
	$generic_config{'BR2_LINUX_KERNEL_CUSTOM_REPO_VERSION'} = $opts{'v'};
}

if ($is_64bit) {
	my $rt_path;
	my $runtime_base = $toolchain;

	$runtime_base =~ s|/bin$||;

	if (defined($opts{'3'})) {
		$rt_path = $opts{'3'};
	} else {
		my $arch32 = $arch;

		$arch32 =~ s|64||;
		# "sysroot" and "sys-root" are being used as directory names
		$rt_path = `ls -d "$runtime_base/$arch32"*/sys*root 2>/dev/null`;
		chomp($rt_path);
	}

	if ($rt_path eq '') {
		print("32-bit libraries not found, disabling 32-bit ".
			"support...\n".
			"Use command line option -3 <path> to specify your ".
			"32-bit sysroot.\n");
	} else {
		my $arch64 = $arch_config{$arch}{'arch_name'};
		my $rt64_path =
			`ls -d "$runtime_base/$arch64"*/sys*root 2>/dev/null`;
		chomp($rt64_path);

		# If "lib64" in the sys-root is a sym-link, we can't build a
		# 64-bit rootfs with 32-bit support. (There's nowhere to put
		# 32-bit libraries.)
		if (-l "$rt64_path/lib64") {
			print("Aarch64 toolchain is not multi-lib enabled. ".
				"Disabling 32-bit support.\n");
		} else {
			print("Using $rt_path for 32-bit environment\n");
			$arch_config{$arch}{'BR2_ROOTFS_RUNTIME32'} = 'y';
			$arch_config{$arch}{'BR2_ROOTFS_RUNTIME32_PATH'} = $rt_path;
		}
	}
}

write_config($arch_config{$arch}, $temp_config, 1);
write_config($toolchain_config{$arch}, $temp_config, 0);
write_config(\%generic_config, $temp_config, 0);

system("support/kconfig/merge_config.sh -m configs/brcmstb_defconfig ".
	"\"$temp_config\"");
if (defined($opts{'f'})) {
	my $fragment_file = $opts{'f'};
	system("support/kconfig/merge_config.sh -m configs/brcmstb_defconfig ".
		"\"$fragment_file\"");
}
unlink($temp_config);
move_merged_config($prg, $arch, ".config", "configs/$merged_config");

# Finalize the configuration by running make ..._defconfig.
system("make O=\"$br_outputdir\" \"$merged_config\"");

print("Buildroot has been configured for ".uc($arch).".\n");
if (defined($opts{'i'})) {
	print("Launching build, including file system images...\n");
	# The "images" target only exists in the generated Makefile in
	# $br_outputdir, so using "make O=..." does not work here.
	system("make -C \"$br_outputdir\" images");
} elsif (defined($opts{'b'})) {
	print("Launching build...\n");
	system("make O=\"$br_outputdir\"");
} else {
	print("To build it, run the following commands:\n".
	"\tcd $br_outputdir; make\n");
}