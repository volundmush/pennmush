#!/usr/bin/perl 
# perl version of the old mkcmds.sh script. Runs faster by simply not running
# a bazillion child processes.

use strict; # Please ma'am may I have another?
use warnings;
use File::Compare;
use File::Copy;
use File::Temp qw/tempfile/;
# use feature qw/say/; # People STILL sometimes use OSes that have really ancient perl versions that don't support say. Sigh.

my @tmpfiles;

END {
    # Make sure temp files get deleted.
    unlink @tmpfiles;
}

# Main loop, dispatch for each command line argument.
foreach my $command (@ARGV) {
    if ($command eq "switches") {
        make_switches();
    } elsif ($command eq "commands") {
        make_cmds();
    } elsif ($command eq "functions") {
        make_funs();
    } elsif ($command eq "all") {
        make_switches();
        make_cmds();
        make_funs();
    } else {
        warn "Unknown option '${command}'\n";
    }
}

# maybemove(file1, file2) copies file1 to file 2 if they are different,
# otherwise just deletes file1 and leaves file2 unchanged.
sub maybemove {
    my ($from, $to) = @_;

    if (compare $from, $to) {
        if (move $from, $to) {
            print "File ${to} updated.\n";
        } else {
            warn "Couldn't rename ${from} to ${to}: $!\n";
        }
    } else {
        print "File ${to} unchanged.\n";
        unlink $from;
    }
}

# scan_files_for_pattern(glob-pattern, re) searches all files matching
# glob-pattern for lines matching re, and returns a sorted list of
# $1's for each matching line.
sub scan_files_for_pattern {
    my ($filepattern, $re) = @_;
    my @idents;

    foreach my $file (glob $filepattern) {
        open my $FILE, "<", $file
            or die "Cannot open ${file} for reading: $!\n";
        while (<$FILE>) {
            s/\s+$//; # Some versions of perl for windows don't handle line ending conversion properly and chomp leaves a \r at the end. Sigh.
            push @idents, $1 if m/$re/;
        }
        close $FILE;
    }
    return sort @idents;
}

sub make_switches {
    print "Rebuilding command switch file and header.\n";

    my ($HDR, $temphdr) = tempfile();
    my ($SRC, $tempsrc) = tempfile();

    push @tmpfiles, $temphdr, $tempsrc;
    
    my @switches = scan_files_for_pattern "src/SWITCHES", qr/^(.+)/;

    print $HDR <<EOH;
/* AUTOGENERATED FILE. DO NOT EDIT! */
#ifndef SWITCHES_H
#define SWITCHES_H
EOH

    my $nswitches = @switches;
    my $maxswitch = $nswitches;
    $nswitches += 1;

    print $SRC <<EOS;
/* AUTOGENERATED FILE. DO NOT EDIT! */
static const int max_switch = ${maxswitch};
SWITCH_VALUE switch_list[${nswitches}] = {
EOS

    my $n = 1;
    foreach my $switch (@switches) {
        print $HDR "#define SWITCH_${switch} ${n}\n";
        print $SRC "  {\"${switch}\", SWITCH_${switch}, 0},\n";
        $n++;
    }

    print $SRC <<EOS;
  {NULL, 0, 0}
};
EOS
    close $SRC;

    print $HDR "#endif                          /* SWITCHES_H */\n";
    close $HDR;

    maybemove $temphdr, "hdrs/switches.h";
    maybemove $tempsrc, "src/switchinc.c";
}

# I really should combine this and make_funs into one function that does
# the work with specific files/regexps/defines passed as arguments

sub make_cmds {
    print "Rebuilding command prototype header.\n";

    my ($HDR, $tempfile) = tempfile();

    push @tmpfiles, $tempfile;

    my @commands =
        scan_files_for_pattern "src/*.c", qr/^\s*COMMAND\(([^\)]+)\)/;

    print $HDR <<EOH;
/* AUTOGENERATED FILE. DO NOT EDIT! */
#ifndef CMDS_H
#define CMDS_H
#include "command.h"
EOH

    foreach my $command (@commands) {
        print $HDR "COMMAND_PROTO(${command});\n";
    }

    print $HDR "#endif /* CMDS_H */\n";
    close $HDR;

    maybemove $tempfile, "hdrs/cmds.h";

}

sub make_funs {
    print "Rebuilding function prototype header.\n";

    my ($HDR, $tempfile) = tempfile();
    my @functions =
        scan_files_for_pattern "src/*.c", qr/^\s*FUNCTION\(([^\)]+)\)/;

    push @tmpfiles, $tempfile;
    
    print $HDR <<EOH;
/* AUTOGENERATED FILE. DO NOT EDIT! */
#ifndef FUNS_H
#define FUNS_H
EOH

    foreach my $function (@functions) {
        print $HDR "FUNCTION_PROTO(${function});\n";
    }

    print $HDR "#endif /* FUNS_H */\n";
    close $HDR;

    maybemove $tempfile, "hdrs/funs.h";
}

