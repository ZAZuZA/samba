#!/usr/bin/perl
# Bootstrap Samba and run a number of tests against it.
# Copyright (C) 2005-2007 Jelmer Vernooij <jelmer@samba.org>
# Published under the GNU GPL, v3 or later.

package Samba;

use strict;
use target::Samba3;
use target::Samba4;

sub new($$$$$) {
	my ($classname, $bindir, $binary_mapping, $bindir_path, $ldap, $srcdir, $exeext, $server_maxtime) = @_;

	my $self = {
	    samba3 => new Samba3($bindir,$binary_mapping, $bindir_path, $srcdir, $exeext, $server_maxtime),
	    samba4 => new Samba4($bindir,$binary_mapping, $bindir_path, $ldap, $srcdir, $exeext, $server_maxtime),
	};
	bless $self;
	return $self;
}

sub setup_env($$$)
{
	my ($self, $envname, $path) = @_;

	$ENV{ENVNAME} = $envname;

	my $env = $self->{samba4}->setup_env($envname, $path);
	if (defined($env)) {
	    $env->{target} = $self->{samba4};
	} else {
	   	$env = $self->{samba3}->setup_env($envname, $path);
		if (defined($env)) {
		    $env->{target} = $self->{samba3};
		}
	}
	if (not defined $env) {
		warn("Samba can't provide environment '$envname'");
		return undef;
	}
	return $env;
}

1;