/***************************************************************************
 * LPRng - An Extended Print Spooler System
 *
 * Copyright 1988-2003, Patrick Powell, San Diego, CA
 *     papowell@lprng.com
 * See LICENSE for conditions of use.
 *
 ***************************************************************************/

#include "lp.h"
#include "getopt.h"
#include "checkpc.h"
#include "getprinter.h"
#include "getqueue.h"
#include "initialize.h"
#include "lockfile.h"
#include "fileopen.h"
#include "child.h"
#include "stty.h"
#include "proctitle.h"
#include "lpd_remove.h"
#include "linksupport.h"
#include "gethostinfo.h"

/**** ENDINCLUDE ****/

 
static int Noaccount;
static int Nolog,  Nostatus,  Fix,  Age, Printcap;
static int Truncate = -1;
static int Remove;
static char *User_specified_printer;
static time_t Current_time;
static int Check_path_list( char *plist, int allow_missing );


/* pathnames of the spool directory (sd) and control directory (cd) */

int main( int argc, char *argv[], char *envp[] )
{
	int i, c, found_pc;
	char *path;		/* end of string */
	int ruid, euid, rgid, egid;
	char *printcap;
	/*char *serial_line = 0;*/
	struct line_list raw, spooldirs;
	const char *s, *t;
	char *p;
	struct stat statb;

	(void)signal( SIGPIPE, SIG_IGN );
	(void)signal( SIGCHLD, SIG_DFL );

	s = t = printcap = 0;
	Init_line_list(&raw);
	Init_line_list(&spooldirs);
	/* set up the uid state */
	To_euid_root();
	time(&Current_time);

	Verbose = 0;
	Warnings = 1;
	Is_server = 1;
	/* send trace on STDOUT */ 
	dup2(1,2);

#ifndef NODEBUG
	Debug = 0;
#endif

	s = t = 0;
	for( i = 0; Pc_var_list[i].keyword;  s = t, ++i ){
		t = Pc_var_list[i].keyword;
		if( s && t && strcmp(s,t) >= 0 ){
			FPRINTF(STDERR, "Pc_var_list: '%s' >= '%s'\n", s, t );
		}
	}


    /* scan the argument list for a 'Debug' value */
	while( (c = Getopt( argc, argv, "aflprst:A:CD:P:T:V" ) ) != EOF ){
		switch( c ){
			default: usage();
			case 'a': Noaccount = 1; break;
			case 'f': Fix = 1; break;
			case 'l': Nolog = 1; break;
			case 'r': Remove = 1; break;
			case 's': Nostatus = 1; break;
			case 't':
				if( Optarg ){
					 Truncate = getk( Optarg );
				} else {
					usage();
				}
				break;
			case 'A':
				if( Optarg){
					Age = getage( Optarg );
				} else {
					usage();
				}
				break;
			case 'D': Parse_debug(Optarg,1); break;
			case 'V': ++Verbose; break;
			case 'p': ++Printcap; break;
			case 'P': User_specified_printer = Optarg; break;
			case 'T':
				initsetproctitle( argc, argv, envp );
				Test_port( getuid(), geteuid(), Optarg );
				exit(0);
				break;
		}
	}

	if( Verbose ){
		if(Verbose)MESSAGE("%s", Version);
	}

	Initialize(argc, argv, envp, 'D' );
	Setup_configuration();

	To_daemon();

	/* we have a user specified printcap we can check as well */
	Free_line_list(&raw);
	for( i = Optind; i < argc; ++i ){
		Getprintcap_pathlist( Require_configfiles_DYN,
			&raw, &PC_filters_line_list,
			argv[i] );
	}
	Build_printcap_info( &PC_names_line_list, &PC_order_line_list,
		&PC_info_line_list, &raw, &Host_IP );
	Free_line_list( &raw );

#if defined (__CYGWIN__)
	if( Fix && (geteuid() != ROOTUID && getuid() != ROOTUID) ){
#else
	if( Fix && geteuid() && getuid() ){
#endif
		WARNMSG("Fix option (-f) requires root permissions\n" );
	}

	if(Verbose)MESSAGE("Checking for configuration files '%s'", Config_file_DYN);
	found_pc = Check_path_list( Config_file_DYN, 0 );
	if( found_pc == 0 ){
		WARNMSG("No configuration file found in '%s'", Config_file_DYN );
	}

	if(Verbose)MESSAGE("Checking for printcap files '%s'", Printcap_path_DYN);
	if( Is_server && Lpd_printcap_path_DYN ){
		if(Verbose)MESSAGE("Checking for lpd only printcap files '%s'", Lpd_printcap_path_DYN);
		found_pc += Check_path_list( Lpd_printcap_path_DYN, 1 );
	} else {
		found_pc += Check_path_list( Printcap_path_DYN, 0 );
	}
	if( found_pc == 0 ){
		WARNMSG("No printcap files!!!" );
	}

	Get_all_printcap_entries();

	euid = geteuid();
	ruid = getuid();
	egid = getegid();
	rgid = getgid();


	DEBUG1("Effective UID %d, Real UID %d, Effective GID %d, Real GID %d",
		euid, ruid, egid, rgid );
	if(Verbose)MESSAGE(" DaemonUID %ld, DaemonGID %ld", (long)DaemonUID, (long)DaemonGID );


	if(Verbose )MESSAGE("Using Config file '%s'", Config_file_DYN);
	/* print errors and everything on STDOUT */

	if( Lockfile_DYN == 0 ){
		WARNMSG( "Warning: no LPD lockfile" );
	} else if( Lpd_port_DYN == 0 ){
		WARNMSG( "Warning: no LPD port" );
	} else {
		int oldfile = Spool_file_perms_DYN;
		Spool_file_perms_DYN = 0644;
		path = safestrdup3( Lockfile_DYN,".", Lpd_port_DYN, __FILE__, __LINE__ ); 
		if(Verbose)MESSAGE( "LPD lockfile '%s'", path );
		if( path[0] != '/' ){
			WARNMSG( "Warning: LPD lockfile '%s' not absolute path", path );
		} else if( !(p = safestrrchr(path+1,'/')) ){
			WARNMSG( "Warning: bad LPD lockfile '%s' path format", path );
		} else {
			*p = 0;
			if( stat( path, &statb ) ){
				WARNMSG( "  LPD Lockfile directory '%s' does not exist!", path);
				if( Fix ){
					mkdir_path( path );
				}
			}
			*p = '/';
		}
		free( path ); path = 0;
		Spool_file_perms_DYN = oldfile;
	}

	if( Verbose )Show_all_printcap_entries();

    if(DEBUGL3){
		struct stat statb; int i;
        LOGDEBUG("main: START open fd's");
        for( i = 0; i < 20; ++i ){
            if( fstat(i,&statb) == 0 ){
                LOGDEBUG("  fd %d (0%o)", i, (unsigned int)(statb.st_mode&S_IFMT));
            }
        }
    }
	if(Verbose)MESSAGE("Checking printcap info");
	if( User_specified_printer ){
		if( DEBUGL1 ) Dump_line_list("checkpc: names", &PC_names_line_list );
		s = Find_str_value( &PC_names_line_list, User_specified_printer );
		DEBUG1("checkpc: for SERVER %s is really %s", User_specified_printer, s );
		if( s ){
			Set_DYN(&Printer_DYN,s);
			Scan_printer(&spooldirs);
		}
	} else {
		if( DEBUGL1 ) Dump_line_list("checkpc: all", &All_line_list );
		for( i = 0; i < All_line_list.count; ++i ){
			Set_DYN(&Printer_DYN,All_line_list.list[i]);
			Scan_printer(&spooldirs);
		}
	}

    if(DEBUGL3){
		struct stat statb; int i;
        LOGDEBUG("main: END open fd's");
        for( i = 0; i < 20; ++i ){
            if( fstat(i,&statb) == 0 ){
                LOGDEBUG("  fd %d (0%o)", i, (unsigned int)(statb.st_mode&S_IFMT));
            }
        }
    }
	Free_line_list(&raw);
	Free_line_list(&spooldirs);
	return(0);
}

void mkdir_path( char *path )
{
	struct stat statb;
	char *s;

	if( ! stat(path,&statb) ){
		s = strrchr(path,'/');
		if( s ){
			*s = 0;
			mkdir_path(path);
			*s = '/';
			if( mkdir( path, 0755 ) ){
				FPRINTF(STDERR,"You cannot mkdir %s - something is wrong",
						path );
				exit(1);
			}
		} else {
			FPRINTF(STDERR,"You cannot stat %s - something is wrong",
				path );
			exit(1);
		}
		
	}
}

/***************************************************************************
 * Scan_printer()
 * process the printer spool queue
 * 1. get the spool queue entry
 * 2. check to see if there is a spool dir
 * 3. perform checks for various files existence and permissions
 ***************************************************************************/

 /* check for these names and values */
 static const char *filter_names[] = {
	"filter", "bp", "bs", "be", 0
	};

void Scan_printer(struct line_list *spooldirs)
{
	DIR *dir;
	struct dirent *d;
	char *s, *cf_name;		/* ACME pointers */
	const char *cs, **names;
	int jobfile;
	struct stat statb;
	int fd = 0;				/* device file descriptor */
	int i, n, fifo_header_len;
	char error[SMALLBUFFER];
	int errorlen = sizeof(error);
	struct job job;
	time_t delta;

	fifo_header_len = safestrlen( Fifo_lock_file_DYN );
	Init_job(&job);
	error[0] = 0;

	if(Verbose)MESSAGE( "Checking printer '%s'", Printer_DYN );

    if(DEBUGL3){
		struct stat statb; int i;
        LOGDEBUG("Scan_printer: START open fd's");
        for( i = 0; i < 20; ++i ){
            if( fstat(i,&statb) == 0 ){
                LOGDEBUG("  fd %d (0%o)", i, (unsigned int)(statb.st_mode&S_IFMT));
            }
        }
    }

	/* get printer information */
	error[0] = 0;
	Fix_Rm_Rp_info(error, sizeof(error) );
	if( error[0] ){
		WARNMSG(
			"%s: '%s'",
			Printer_DYN, error );
	}
	if( !Is_server ){
		if( !Lp_device_DYN && !RemoteHost_DYN && !Force_localhost_DYN ){
			WARNMSG( "%s: no printer printer information", Printer_DYN);
		}
		if( RemoteHost_DYN && !RemotePrinter_DYN ){
			WARNMSG( "%s: no remote printer information", Printer_DYN);
		}
		goto test_filters;
	}
	if( !Find_first_key(&PC_entry_line_list,"bq",Option_value_sep,&n)
		|| !Find_first_key(&Config_line_list,"bq",Option_value_sep,&n ) ){
		WARNMSG( "%s: bq option is no longer supported, use 'lpd_bounce' option", Printer_DYN);
	}
	if( !Find_first_key(&PC_entry_line_list,"check_idle",Option_value_sep,&n)
		|| !Find_first_key(&Config_line_list,"check_idle",Option_value_sep,&n ) ){
		WARNMSG( "%s: check_idle option is no longer supported, use 'chooser' option", Printer_DYN);
	}
	if( !Find_first_key(&PC_entry_line_list,"sf",Option_value_sep,&n)
		|| !Find_first_key(&Config_line_list,"sf",Option_value_sep,&n ) ){
		WARNMSG( "%s: sf (suppress form feeds) is deprecated.  Use 'ff_separator' if you want FF between job files", Printer_DYN);
	}
	if( strchr(Printer_DYN, '*') ){
		WARNMSG(
			"printcap entry '%s': Wildcard entry cannot be a server queue name, use :client to mark for client or use wildcard as alias",
			Printer_DYN);
		return;
	}

	Setup_printer( Printer_DYN, error, errorlen, 0);
	DEBUG3(
	"Scan_printer: Printer_DYN '%s', RemoteHost_DYN '%s', RemotePrinter_DYN '%s', Lp '%s'",
		Printer_DYN, RemoteHost_DYN, RemotePrinter_DYN, Lp_device_DYN );
	/* check to see if printer defined in database */
	if( Spool_dir_DYN == 0 ){
		WARNMSG("%s: Bad printcap entry - missing 'sd' or 'client' entry?",
			Printer_DYN);
		return;
	}
	if( (s =  Find_str_value(spooldirs,Spool_dir_DYN)) ){
		WARNMSG("%s: CATASTROPHIC ERROR! queue '%s' also has spool directory '%s'",
			Printer_DYN, s, Spool_dir_DYN);
		return;
	}
	Set_str_value(spooldirs,Spool_dir_DYN,Printer_DYN);
	

	/*
	 * check the permissions of files and directories
	 * Also remove old job or control files
	 */
	if( Check_spool_dir( Spool_dir_DYN ) > 1 ){
		WARNMSG( "  Printer_DYN '%s' spool dir '%s' needs fixing",
			Printer_DYN, Spool_dir_DYN );
		return;
	}
	if( !(dir = opendir( Spool_dir_DYN )) ){
		WARNMSG( "  Printer_DYN '%s' spool dir '%s' cannot be scanned '%s'",
			Printer_DYN, Spool_dir_DYN, Errormsg(errno) );
		return;
	}
	if( (Fix || Remove) && Lpq_status_file_DYN ){
		unlink(Lpq_status_file_DYN);
	}
	while( (d = readdir(dir)) ){
		cf_name = d->d_name;
		if( safestrcmp( cf_name, "." ) == 0
			|| safestrcmp( cf_name, ".." ) == 0 ) continue;
		DEBUG2("Scan_printer: file '%s'", cf_name );
		if( fifo_header_len &&
			!safestrncmp( cf_name,Fifo_lock_file_DYN, fifo_header_len) ){
			DEBUG2("Scan_printer: fifo file '%s'", cf_name );
			unlink( cf_name );
			continue;
		}
		if( stat(cf_name,&statb) == -1 ){
			WARNMSG( "  stat of file '%s' failed '%s'",
				cf_name, Errormsg(errno) );
			continue;
		}
		/* do not touch symbolic links */
		if( S_ISLNK( statb.st_mode ) ){
			continue;
		}
		delta = Current_time - statb.st_mtime;

		/*
		 * cfA000 -> cXXn
		 * dfA000 -> cXXn
		 * hfA000 -> cXXn
		 */
		jobfile = (
				   strchr( "cdh", cf_name[0] )
				&& isalpha(cval(cf_name+1))
				&& isalpha(cval(cf_name+2))
				&& isdigit(cval(cf_name+3)) );

		if( jobfile && Age && delta > Age ){
			float n = (delta)/60.0 ;
			float a = (Age)/60.0 ;
			const char *remove = Remove?" (removing)":"";
			const char *range = "mins";
			if( a/60 > 2 ){
				a = a/60;
				n = n/60;
				range = "hours";
				if( a/24 > 2 ){
					a = a/24;
					n = n/24;
					range = "days";
				}
			}
            if( (statb.st_size == 0) ){
				if( Remove || Verbose)MESSAGE( " %s:  file '%s', zero length file > %3.2f %s old%s",
					Printer_DYN, cf_name, n, range, remove );
				if( Remove ){
					unlink(cf_name);
				}
				continue;
			} else {
				if( Remove || Verbose)MESSAGE( " %s:  file '%s', age %3.2f %s > %3.2f %s maximum%s",
					Printer_DYN, cf_name, n, range, a, range, remove );
				if( Remove ){
					unlink(cf_name);
				}
				continue;
			}
		}
		/* we update all real files in this directory */
		if( jobfile ){
			Check_file( cf_name, Fix, 0, 0 );
		}
	}
	closedir(dir);

	Make_write_file( Queue_control_file_DYN, 0 );
	Make_write_file( Queue_status_file_DYN, 0 );
	Fix_clean(Status_file_DYN,Nostatus);
	Fix_clean(Log_file_DYN,Nolog);
	Fix_clean(Accounting_file_DYN,Noaccount);
	if( (s = Ppd_file_DYN) ){
		Check_read_file( s, Fix, 0644 );
	}
	/*
	 * get the jobs in the queue
	 */
	if( Fix ){
		if( Lpq_status_file_DYN ) unlink(Lpq_status_file_DYN );
	}
	Free_line_list( &Sort_order );
	{ int fdx = open("/dev/null",O_RDWR); DEBUG1("Scan_printer: Scan_queue before maxfd %d", fdx); close(fdx); }
	Scan_queue( &Spool_control, &Sort_order,0,0,0,0, 0, 0,0,0 );
	{ int fdx = open("/dev/null",O_RDWR); DEBUG1("Scan_printer: Scan_queue after maxfd %d", fdx); close(fdx); }

	/*
	 * check to see if we have a local or remote printer
	 * do not check if name has a | or % character in it
	 */

		/* we should have a local printer */
	if( Server_queue_name_DYN == 0
		&& RemotePrinter_DYN == 0 && Lp_device_DYN == 0 ){
		WARNMSG( "Missing 'lp' and 'rp' entry for local printer" );
	}
 test_filters:
	if( Lp_device_DYN && safestrpbrk( Lp_device_DYN, "|%@" ) == 0  ){
		s = Lp_device_DYN;
		fd = -1;
		if( s[0] != '/' ){
			WARNMSG( "%s: lp device not absolute  pathname '%s'",
				Printer_DYN, s );
		} else if( stat(s,&statb) < 0 ){
			WARNMSG( "%s: cannot stat lp device '%s' - %s",
				Printer_DYN, s, Errormsg(errno) );
		} else if( (fd = Checkwrite(s,&statb,0,0,1)) < 0 ){
			WARNMSG( "%s: cannot open lp device '%s' - %s",
				Printer_DYN, s, Errormsg(errno) );
		}
		if( fd >= 0 ) close(fd);
	}

	/* check the filters */
	strcpy(error,"xf");
	for( i = 'a'; i <= 'z'; ++i ){
		if( safestrchr("afls",i) ) continue;
		error[0] = i;
		Check_executable_filter( error, 0 );
	}

	for( names = filter_names; (cs = *names); ++names ){
		Check_executable_filter( cs, 0 );
	}

	/* check the Lpd_port_DYN */
	n = 0;
	if( (s = safestrchr( Lpd_port_DYN, '%')) ){
		n = Link_dest_port_num(s+1);
	} else if( Lpd_port_DYN ){
		n = Link_dest_port_num(Lpd_port_DYN);
	}
	if( n == 0 ){
		WARNMSG( "%s: bad lpd_port value '%s'",
			Printer_DYN, Lpd_port_DYN );
	}
}

void Check_executable_filter( const char *id, char *filter_str )
{
	struct line_list files;
	char *s, *t;
	struct stat statb;
	int c, j, n;
	
	Init_line_list(&files);
	if( !filter_str ){
		filter_str = Find_str_value(&PC_entry_line_list,id);
		if(!filter_str) filter_str = Find_str_value(&Config_line_list,id);
	}
	Split(&files,filter_str,Whitespace,0,0,0,0,0,0);
	if( files.count ){
		if(Verbose)MESSAGE("  '%s' filter '%s'", id, filter_str );
		s = 0;
		for( j = 0; j < files.count; ++j ){
			s = files.list[j];
			while(s && (c =cval(s)) ){
			if( isspace(c) ){ ++s; continue; };
			if( c == '|' ){ ++s; continue; };
			if( !safestrncasecmp(s,"$-",2) ){ s+=2; continue;}
			if( !safestrncasecmp(s,"-$",2) ){ s+=2; continue;}
			if( !safestrncasecmp(s,"root",4) ){ s+=4; continue;}
			break;
			}
			if( *s ) break;
		}
		c = cval(s);
		if( c == '(' || strpbrk( s, "<>|;") ){
			if(Verbose)MESSAGE("    shell script '%s'", filter_str );
			t = filter_str + safestrlen(filter_str) - 1;
			while( isspace(cval(t)) ) --t;
			if( cval(t) != ')' ){
				WARNMSG("filter needs ')' at end - '%s'", filter_str );
			}
			if( c == '(' ) ++s;
			while( isspace(cval(s)) ) ++s;
			c = cval(s);
			if( c != '/' ) goto exit;
		}
		if(Verbose)MESSAGE("    executable '%s'", s );
		if( stat(s,&statb) ){
			WARNMSG("cannot stat '%s' filter '%s' - %s", id,
			s, Errormsg(errno) );
		} else if(!S_ISREG(statb.st_mode)) {
			WARNMSG("'%s' filter '%s' not a file", id, s);
		} else {
			n = statb.st_mode & 0111;
			if( !(n & 0001)
				&& !((n & 0010) && statb.st_gid == DaemonGID )
				&& !((n & 0100) && statb.st_uid == DaemonUID ) ){
				WARNMSG("'%s' filter '%s' does not have execute perms",
					id, s );
			}
		}
	}
 exit:
	Free_line_list( &files );
}

/***************************************************************************
 * Make_write_file(
 *    dir - directory name
 *    name - file name
 *    printer - if non-zero, append to end of file name
 ***************************************************************************/

void Make_write_file( char *file, char *printer )
{
	int fd;
	struct stat statb;
	char *s;

	if( file == 0 || *file == 0 ){
		return;
	}
	s = safestrdup2(file,printer,__FILE__,__LINE__);
	DEBUG1("Make_write_file '%s'", s );
	if( Verbose || DEBUGL1 ){
		if(Verbose)MESSAGE( "  checking '%s' file", s );
	}

	if( (fd = Checkwrite( s, &statb, O_RDWR, 1, 1 )) < 0 ){
		WARNMSG( " ** cannot open '%s' - '%s'", s, Errormsg(errno) );
		if( Fix ){
			int euid = geteuid();
			To_euid_root();
			fd = open( s, O_RDWR|O_CREAT, Spool_file_perms_DYN  );
			To_euid(euid);
			if( fd < 0 ){
				WARNMSG( " ** cannot create '%s' - '%s'", s, Errormsg(errno) );
			}
			Fix_owner( s );
		}
	}
	if( Check_file( s, Fix, 0, 0 ) ){
		WARNMSG("  ** ownership or permissions problem with '%s'", s );
	}
	free(s); s = 0;
	if( fd >= 0 ) close(fd);
	fd = -1;
}

 __attribute__((noreturn)) static void usage(void) 
{
	FPRINTF( STDERR,
"checkpc [-aflprsV] [-A age] [-D debuglevel] [-P printer] [-t size]\n"
"   Check printcap for printer information and fix files where possible\n"
" Option:\n"
" -a             do not create accounting info (:af) file\n"
" -f             fix missing files and inconsistent file permissions\n"
" -l             do not create logging info (:lf) file\n"
" -p             verbose printcap information\n"
" -r             remove job files older than -A age seconds\n"
" -s             do not create filter status (:ps) info file\n"
" -t size[kM]    truncate log files (:lf) to size (k=Kbyte, M=Mbytes)\n"
" -A age[DHMS]   remove files of form ?f[A-Z][0-9][0-9][0-9] older than\n"
"                age, D days (default), H hours, M minutes, S seconds\n"
" -D debuglevel  set debug level\n"
" -P printer     check or fix only this printer entry\n"
" -V             really verbose information\n"
" -T line        portability diagnostic, use serial line device for stty test\n");
	Parse_debug("=",-1);
	FPRINTF( STDOUT, "%s\n", Version );
	exit(1);
}

int getage( char *age )
{
	int t;
	char *end = age;
	t = strtol( age, &end, 10 );
	if( t && end != age ){
		switch( *end ){
			default: t = 0; break;

			case 0:
			case 'd': case 'D': t *= 24;  __attribute__((fallthrough));
			case 'h': case 'H': t *= 60; __attribute__((fallthrough));
			case 'm': case 'M': t *= 60; __attribute__((fallthrough));
			case 's': case 'S': break;
		}
	}
	if( t == 0 ){
		FPRINTF( STDERR, "Bad format for age '%s'", age );
		usage();
	}
	return t;
}


int getk( char *age )
{
	int t;
	char *end = age;
	t = strtol( age, &end, 10 );
	if( end != age ){
		switch( *end ){
			default:
				FPRINTF( STDERR, "Bad format for number '%s'", age );
				usage();
			case 0: break;
			case 'k': case 'K': break;
			case 'm': case 'M': t *= (1024); break;
		}
	}
	return t;
}

/***************************************************************************
 * Check_file( char  *dpath   - pathname of directory/files
 *    int fix  - fix or check
 *    int t    - time to compare against
 *    int age  - maximum age of file
 ***************************************************************************/

int Check_file( char  *path, int fix, int age, int rmflag )
{
	struct stat statb;
	int old;
	int err = 0;

	DEBUG4("Check_file: '%s', fix %d, time 0x%lx, age %d",
		path, fix, (long)Current_time, age );

	if( stat( path, &statb ) ){
		WARNMSG( "  %s: cannot stat file '%s', %s", Printer_DYN?Printer_DYN:"", path, Errormsg(errno) );
		err = 1;
		return( err );
	}
	if( S_ISDIR( statb.st_mode ) ){
		WARNMSG("  %s: '%s' is a directory, not a file", Printer_DYN?Printer_DYN:"",path );
		return(2);
	} else if( !S_ISREG( statb.st_mode ) ){
		WARNMSG( " %s: '%s' not a regular file - unusual", Printer_DYN?Printer_DYN:"",path );
		return(2) ;
	}

	if( statb.st_uid != DaemonUID || statb.st_gid != DaemonGID ){
		WARNMSG( "owner/group of '%s' are %ld/%ld, not %ld/%ld", path,
			(long)(statb.st_uid), (long)(statb.st_gid), (long)DaemonUID, (long)DaemonGID );
		if( fix ){
			if( Fix_owner( path ) ) err = 2;
		}
	}
	if( 07777 & (statb.st_mode ^ Spool_file_perms_DYN) ){
		WARNMSG( "permissions of '%s' are 0%o, not 0%o", path,
			(unsigned int)(statb.st_mode & 07777), Spool_file_perms_DYN );
		if( fix ){
			if( Fix_perms( path, Spool_file_perms_DYN ) ) err = 1;
		}
	}
	if( age ){
		old = Current_time - statb.st_ctime;
		if( old >= age ){
			FPRINTF( STDOUT,
				"file %s age is %d secs, max allowed %d secs\n",
				path, old, age );
			if( rmflag ){
				FPRINTF( STDOUT, "removing '%s'\n", path );
				if( unlink( path ) == -1 ){
					WARNMSG( "cannot remove '%s', %s", path,
						Errormsg(errno) );
				}
			}
		}
	}
	return( err );
}


/***************************************************************************
 * Check_read_file( char  *dpath   - pathname of directory/files
 *    int fix  - fix or check
 ***************************************************************************/

int Check_read_file( char  *path, int fix, int perms )
{
	struct stat statb;
	int err = 0;
	int fd;

	DEBUG4("Check_read_file: '%s', fix %d", path, fix );

	if( stat( path, &statb ) ){
		WARNMSG( "  %s: cannot stat file '%s', %s", Printer_DYN?Printer_DYN:"",
			path, Errormsg(errno) );
		err = 1;
		return( err );
	}
	if( S_ISDIR( statb.st_mode ) ){
		WARNMSG("  %s: '%s' is a directory, not a file", Printer_DYN?Printer_DYN:"",path );
		return(2);
	} else if( !S_ISREG( statb.st_mode ) ){
		WARNMSG( " %s: '%s' not a regular file - unusual", Printer_DYN?Printer_DYN:"",path );
		return(2) ;
	}
	if( (fd = Checkread( path, &statb )) < 0 ){
		if( fix ){
			Fix_perms( path, perms );
		} else {
			WARNMSG( " %s: cannot open %s - %s", Printer_DYN?Printer_DYN:"",
				path, Errormsg(errno) );
		}
	}
	if( fd >= 0 ) close(fd);

	return( err );
}

int Fix_create_dir( char  *path, struct stat *statb )
{
	char *s;
	int err = 0;

	s = path+safestrlen(path)-1;
	if( *s == '/' ) *s = 0;
	if( stat( path, statb ) == 0 ){
		if( !S_ISDIR( statb->st_mode ) ){
			if( !S_ISREG( statb->st_mode ) ){
				WARNMSG( "not regular file '%s'", path );
				err = 1;
			} else if( unlink( s ) ){
				WARNMSG( "cannot unlink file '%s', %s",
					path, Errormsg(errno) );
				err = 1;
			}
		}
	}
	/* we don't have a directory */
	if( stat( path, statb ) ){
		int euid = geteuid();
		To_euid_root();
		if( mkdir( path, Spool_dir_perms_DYN ) ){
			WARNMSG( "mkdir '%s' failed, %s", path, Errormsg(errno) );
			err = 1;
		} else {
			err = Fix_owner( path );
		}
		To_euid(euid);
	}
	return( err );
}

int Fix_owner( char *path )
{
	int status = 0;
	int err;
	int euid = geteuid();

	To_euid_root();
	WARNMSG( "  changing ownership '%s' to %ld/%ld", path, (long)DaemonUID, (long)DaemonGID );
	chown( path, DaemonUID, DaemonGID );
	if( geteuid() == ROOTUID ){
		WARNMSG( "  changing ownership '%s' to %ld/%ld", path, (long)DaemonUID, (long)DaemonGID );
		status = chown( path, DaemonUID, DaemonGID );
		err = errno;
		if( status ){
			WARNMSG( "chown '%s' failed, %s", path, Errormsg(err) );
		}
		errno = err;
	}
	To_euid(euid);
	return( status != 0 );
}

int Fix_perms( char *path, int perms )
{
	int status;
	int err;
	int euid = geteuid();

	To_euid_root();
	status = chmod( path, perms );
	err = errno;
	To_euid( euid );

	if( status ){
		WARNMSG( "chmod '%s' to 0%o failed, %s", path, perms,
			 Errormsg(err) );
	}
	errno = err;
	return( status != 0 );
}


/***************************************************************************
 * Check to see that the spool directory exists, and create it if necessary
 ***************************************************************************/

int Check_spool_dir( char *path )
{
	struct stat statb;
	struct line_list parts;
	char *pathname = 0;
	int err = 0, i;

	/* get the required group and user ids */
	
	if(Verbose)MESSAGE(" Checking directory: '%s'", path );
	pathname = path+safestrlen(path)-1;
	if( pathname[0] == '/' ) *pathname = 0;
	Init_line_list(&parts);
	if( path == 0 || path[0] != '/' || strstr(path,"/../") ){
		WARNMSG("bad spooldir path '%s'", path );
		return(2);
	}

	pathname = 0;
	Split(&parts,path,"/",0,0,0,0,0,0);

	for( i = 0; i < parts.count; ++i ){
		pathname = safeextend3(pathname,"/",parts.list[i],__FILE__,__LINE__);
		if(Verbose)MESSAGE("   directory '%s'", pathname);
		if( stat( pathname, &statb ) || !S_ISDIR( statb.st_mode ) ){
			if( Fix ){
				if( Fix_create_dir( pathname, &statb ) ){
					return(2);
				}
			} else {
				WARNMSG(" bad directory - %s", pathname );
				return( 2 );
			}
		}
		if( stat( pathname, &statb ) == 0 && S_ISDIR( statb.st_mode )
			&& chdir( pathname ) == -1 ){
			if( !Fix ){
				WARNMSG( "cannot chdir to '%s' as UID %ld, GRP %ld - '%s'",
					pathname, (long)geteuid(), (long)getegid(), Errormsg(errno) );
			} else {
				Fix_perms( pathname, Spool_dir_perms_DYN );
				if( chdir( pathname ) == -1 ){
					WARNMSG( "Permission change FAILED: cannot chdir to '%s' as UID %ld, GRP %ld - '%s'",
					pathname, (long)geteuid(), (long)getegid(), Errormsg(errno) );
					Fix_owner( pathname );
					Fix_perms( pathname, Spool_dir_perms_DYN );
				}
				if( chdir( pathname ) == -1 ){
					WARNMSG( "Owner and Permission change FAILED: cannot chdir to '%s' as UID %ld, GRP %ld - '%s'",
					pathname, (long)geteuid(), (long)getegid(), Errormsg(errno) );
				}
			}
		}
	}
	free(pathname); pathname = 0;
	Free_line_list(&parts);
	/* now we do chown if necessary */
	if( Fix ){
		char cmd[SMALLBUFFER];
		int euid = geteuid();

		To_euid_root();
		plp_snprintf( cmd, sizeof(cmd), "%s -R %ld %s", CHOWN, (long)DaemonUID, path );
		system( cmd );
		plp_snprintf( cmd, sizeof(cmd), "%s -R %ld %s", CHGRP, (long)DaemonGID, path );
		system( cmd );
		To_euid(euid);
	}
	if( stat( path, &statb ) ){
		WARNMSG( "stat of '%s' failed - %s", path, Errormsg(errno) );
		err = 1;
		return( err );
	}
	/* now we look at the last directory */
	if( statb.st_uid != DaemonUID || statb.st_gid != DaemonGID ){
		WARNMSG( "owner/group of '%s' are %ld/%ld, not %ld/%ld", path,
			(long)(statb.st_uid), (long)(statb.st_gid), (long)DaemonUID, (long)DaemonGID );
		err = 1;
		if( Fix ){
			if( Fix_owner( path ) ) err = 2;
		}
	}
	if( 07777 & (statb.st_mode ^ Spool_dir_perms_DYN) ){
		WARNMSG( "permissions of '%s' are 0%o, not 0%o", path,
			(unsigned int)(statb.st_mode & 07777), Spool_dir_perms_DYN );
		err = 1;
		if( Fix ){
			if( Fix_perms( path, Spool_dir_perms_DYN ) ) err = 1;
		}
	}
	return(err);
}

/***************************************************************************
 * We have put a slew of portatbility tests in here.
 * 1. setuid
 * 2. RW/pipes, and as a side effect, waitpid()
 * 3. get file system size (/tmp)
 * 4. try nonblocking open
 * 5. try locking test
 * 6. getpid() test
 * 7. try serial line locking
 * 8. try file locking
 ***************************************************************************/

void Test_port(int ruid, int euid, char *serial_line )
{
	FILE *tf;
	char line[LINEBUFFER];
	char cmd[LINEBUFFER];
	char t1[LINEBUFFER];
	char t2[LINEBUFFER];
	char stty[LINEBUFFER];
	char diff[LINEBUFFER];
	const char *sttycmd;
	const char *diffcmd;
	int ttyfd;
	static pid_t pid, result;
	plp_status_t status;
	double freespace;
	static int fd;
	static int i, err;
	struct stat statb;
	/*char *Stty_command;*/

	status = 0;
	fd = -1;

	/*
	 * SETUID
	 * - try to go to user and then back
	 */

	Spool_file_perms_DYN = 000600;
	Spool_dir_perms_DYN =  042700;
	if( ( ruid == ROOTUID && euid == ROOTUID ) || (ruid != ROOTUID && euid != ROOTUID ) ){
			FPRINTF( STDERR,
				"*******************************************************\n" );
			FPRINTF( STDERR, "***** not SETUID, skipping setuid checks\n" );
			FPRINTF( STDERR,
				"*******************************************************\n" );
			goto freespace;
	} else if( ( ruid == ROOTUID || euid == ROOTUID ) ){
		if( UID_root == 0 ){
		FPRINTF( STDERR,
			"checkpc: setuid code failed!! Portability problems\n" );
			exit(1);
		}
		if( To_euid(1) ){
			FPRINTF( STDERR,
			"checkpc: To_euid() seteuid code failed!! Portability problems\n" );
			exit(1);
		}
		if( To_daemon() ){
			FPRINTF( STDERR,
			"checkpc: To_usr() seteuid code failed!! Portability problems\n" );
			exit(1);
		}
		FPRINTF( STDERR, "***** SETUID code works\n" );
	}

 freespace:

	freespace = Space_avail( "/tmp" );

	FPRINTF( STDERR, "***** Free space '/tmp' = %0.0f Kbytes \n"
		"   (check using df command)\n", (double)freespace );

	/*
	 * check serial line
	 */
	if( serial_line == 0 ){
		FPRINTF( STDERR,
			"*******************************************************\n" );
		FPRINTF( STDERR, "********** Missing serial line\n" );
		FPRINTF( STDERR,
			"*******************************************************\n" );
		goto test_lockfd;
	} else {
		FPRINTF( STDERR, "Trying to open '%s'\n",
			serial_line );
		fd = Checkwrite_timeout( 2, serial_line, &statb, O_RDWR, 0, 1 );
		err = errno;
		if( Alarm_timed_out ){
			FPRINTF( STDERR,
				"ERROR: open of '%s'timed out\n"
				" Check to see that the attached device is online\n",
				serial_line );
			goto test_stty;
		} else if( fd < 0 ){
			FPRINTF( STDERR, "Error opening line '%s'\n", Errormsg(err));
			goto test_stty;
		} else if( !isatty( fd ) ){
			FPRINTF( STDERR,
				"*******************************************************\n" );
			FPRINTF( STDERR, "***** '%s' is not a serial line!\n",
				serial_line );
			FPRINTF( STDERR,
				"*******************************************************\n" );
			goto test_stty;
		} else {
			FPRINTF( STDERR, "\nTrying read with timeout\n" );
			i = Read_fd_len_timeout( 1, fd, cmd, sizeof(cmd) );
			err = errno;
			if( Alarm_timed_out ){
				FPRINTF( STDERR, "***** Read with Timeout successful\n" );
			} else {
				 if( i < 0 ){
					FPRINTF( STDERR,
					"***** Read with Timeout FAILED!! Error '%s'\n",
						Errormsg( err ) );
				} else {
					FPRINTF( STDERR,
						"***** Read with Timeout FAILED!! read() returned %d\n",
							i );
		FPRINTF( STDERR,
"***** On BSD derived systems CARRIER DETECT (CD) = OFF indicates EOF condition.\n" );
		FPRINTF( STDERR,
"*****  Check that CD = ON and repeat test with idle input port.\n" );
		FPRINTF( STDERR,
"*****  If the test STILL fails,  then you have problems.\n" );
				}
			}
		}
		/*
		 * now we try locking the serial line
		 */
		/* we try to lock the serial line */
		FPRINTF( STDERR, "\nChecking for serial line locking\n" );
#if defined(LOCK_DEVS) && LOCK_DEVS == 0
		FPRINTF( STDERR,
			"*******************************************************\n" );
		FPRINTF( STDERR,
			"******** Device Locking Disabled by compile time options" );
		FPRINTF( STDERR, "\n" );
		FPRINTF( STDERR,
			"*******************************************************\n" );
		goto test_stty;
#endif

		i = 0;
		if( Set_timeout() ){
			Set_timeout_alarm( 1 );
			i =  LockDevice( fd, 0 );
		}
		Clear_timeout();
		err = errno;
		if( Alarm_timed_out || i < 0 ){
			if( Alarm_timed_out ){
				FPRINTF( STDERR, "LockDevice timed out - %s", Errormsg(err) );
			}
			FPRINTF( STDERR,
				"*******************************************************\n" );
				FPRINTF( STDERR, "********* LockDevice failed -  %s\n",
					Errormsg(err) );
				FPRINTF( STDERR, "********* Try an alternate lock routine\n" );
			FPRINTF( STDERR,
				"*******************************************************\n" );
			goto test_stty;
		}
		
		FPRINTF( STDERR, "***** LockDevice with no contention successful\n" );
		/*
		 * now we fork a child with tries to reopen the file and lock it
		 */
		if( (pid = fork()) < 0 ){
			FPRINTF( STDERR, "fork failed - %s", Errormsg(errno) );
		} else if( pid == 0 ){
			close(fd);
			fd = -1;
			i = -1;
			FPRINTF( STDERR, "Daughter re-opening line '%s'\n", serial_line );
			if( Set_timeout() ){
				Set_timeout_alarm( 1 );
				fd = Checkwrite( serial_line, &statb, O_RDWR, 0, 0 );
				if( fd >= 0 ) i = LockDevice( fd, 1 );
			}
			Clear_timeout();
			err = errno;
			FPRINTF( STDERR, "Daughter open completed- fd '%d', lock %d\n",
				 fd, i );
			if( Alarm_timed_out ){
				FPRINTF( STDERR, "Timeout opening line '%s'\n",
					serial_line );
			} else if( fd < 0 ){
				FPRINTF( STDERR, "Error opening line '%s' - %s\n",
				serial_line, Errormsg(err));
			} else if( i > 0 ){
				FPRINTF( STDERR, "Lock '%s' succeeded! wrong result\n",
					serial_line);
			} else {
				FPRINTF( STDERR, "**** Lock '%s' failed, desired result\n",
					serial_line);
			}
			if( fd >= 0 ){
				FPRINTF( STDERR,"Daughter closing '%d'\n", fd );
				close( fd );
			}
			FPRINTF( STDERR,"Daughter exit with '%d'\n", (i >= 0)  );
			exit(i >= 0);
		} else {
			status = 0;
			FPRINTF( STDERR, "Mother starting sleep\n" );
			plp_usleep(2000);
			FPRINTF( STDERR, "Mother sleep done\n" );
			while(1){
				result = plp_waitpid( -1, &status, 0 );
				err = errno;
				FPRINTF( STDERR, "waitpid result %d, status %d, errno '%s'\n",
					(int)result, status, Errormsg(err) );
				if( result == pid ){
					FPRINTF( STDERR, "Daughter exit status %d\n", status );
					if( status != 0 ){
						FPRINTF( STDERR, "LockDevice failed\n");
					}
					break;
				} else if( (result == -1 && errno == ECHILD) || result == 0 ){
					break;
				} else if( result == -1 && errno != EINTR ){
					FPRINTF( STDERR,
						"plp_waitpid() failed!  This should not happen!");
					status = -1;
					break;
				}
			}
			if( status == 0 ){
				FPRINTF( STDERR, "***** LockDevice() works\n" );
			}
		}
 test_stty:
		/*
		 * do an STTY operation, then print the status.
		 * we cheat and use a shell script; check the output
		 */
		if( fd <= 0 ) goto test_lockfd;
		FPRINTF( STDERR, "\n\n" );
		FPRINTF( STDERR, "Checking stty functions, fd %d\n\n", fd );
		if( (pid = fork()) < 0 ){
			FPRINTF( STDERR, "fork failed - %s", Errormsg(errno) );
		} else if( pid == 0 ){
			/* default for status */
			plp_snprintf( t1, sizeof(t1), "/tmp/t1XXX%ld", (long)getpid() );
			plp_snprintf( t2, sizeof(t2), "/tmp/t2XXX%ld", (long)getpid() );
			diffcmd = "diff -c %s %s 1>&2";
			ttyfd = 1;	/*STDOUT is reported */
			sttycmd = "stty -a 2>%s";	/* on STDERR */
#if defined(SUNOS4_1_4)
			ttyfd = 1;	/*STDOUT is reported */
			sttycmd = "/bin/stty -a 2>%s";	/* on STDERR */
#elif defined(SOLARIS) || defined(SVR4) || defined(linux)
			ttyfd = 0;	/* STDIN is reported */
			sttycmd = "/bin/stty -a >%s";	/* on STDOUT */
#elif (defined(BSD) && (BSD >= 199103))	/* HMS: Might have to be 199306 */
			ttyfd = 0;	/* STDIN is reported */
			sttycmd = "stty -a >%s";	/* on STDOUT */
#elif defined(BSD) /* old style BSD4.[23] */
			sttycmd = "stty everything 2>%s";
#else /* That is: All other System V derivatives and AIX as well */
			ttyfd = 0;	/* STDIN is reported */
			sttycmd = "/bin/stty -a >%s";	/* on STDOUT */
#endif
			if( fd != ttyfd ){
				i = dup2(fd, ttyfd );
				if( i != ttyfd ){
					FPRINTF( STDERR, "dup2() failed - %s\n", Errormsg(errno) );
					exit(-1);
				}
				close( fd );
			}
			plp_snprintf( stty, sizeof(stty), sttycmd, t1 );
			plp_snprintf( diff, sizeof(diff), diffcmd, t1, t2 );
			plp_snprintf( cmd, sizeof(cmd), "%s; cat %s 1>&2", stty, t1 );
			FPRINTF( STDERR,
			"Status before stty, using '%s', on fd %d->%d\n",
				cmd, fd, ttyfd );
			i = system( cmd );
			FPRINTF( STDERR, "\n\n" );
			Stty_command_DYN = "9600 -even odd echo";
			FPRINTF( STDERR, "Trying 'stty %s'\n", Stty_command_DYN );
			Do_stty( ttyfd );
			plp_snprintf( stty, sizeof(stty), sttycmd, t2 );
			plp_snprintf( cmd, sizeof(cmd),
				"%s; %s", stty, diff );
			FPRINTF( STDERR, "Doing '%s'\n", cmd );
			i = system( cmd );
			FPRINTF( STDERR, "\n\n" );
			Stty_command_DYN = "1200 -odd even";
			FPRINTF( STDERR, "Trying 'stty %s'\n", Stty_command_DYN );
			Do_stty( ttyfd );
			FPRINTF( STDERR, "Doing '%s'\n", cmd );
			i = system( cmd );
			FPRINTF( STDERR, "\n\n" );
			Stty_command_DYN = "300 -even -odd -echo cbreak";
			FPRINTF( STDERR, "Trying 'stty %s'\n", Stty_command_DYN );
			Do_stty( ttyfd );
			plp_snprintf( stty, sizeof(stty), sttycmd, serial_line, t2 );
			FPRINTF( STDERR, "Doing '%s'\n", cmd );
			i = system( cmd );
			FPRINTF( STDERR, "\n\n" );
			FPRINTF( STDERR, "Check the above for parity, speed and echo\n" );
			FPRINTF( STDERR, "\n\n" );
			unlink(t1);
			unlink(t2);
			exit(0);
		} else {
			close(fd);
			fd = -1;
			status = 0;
			while(1){
				result = plp_waitpid( -1, &status, 0 );
				if( result == pid ){
					FPRINTF( STDERR, "Daughter exit status %d\n", status );
					if( status != 0 ){
						FPRINTF( STDERR, "STTY operation failed\n");
					}
					break;
				} else if( (result == -1 && errno == ECHILD) || result == 0 ){
					break;
				} else if( result == -1 && errno == EINTR ){
					FPRINTF( STDERR,
						"plp_waitpid() failed!  This should not happen!");
					status = -1;
					break;
				}
			}
			if( status == 0 ){
				FPRINTF( STDERR, "***** STTY works\n" );
			}
		}
	}
 test_lockfd:
	if( fd >= 0 ) close(fd);
	fd = -1;

	FPRINTF( STDERR, "\n\n" );
	/*
	 * check out Lockf
	 */
	plp_snprintf( line, sizeof(line), "/tmp/XX%ldXX", (long)getpid() );
	FPRINTF( STDERR, "Checking Lockf '%s'\n", line );
	if( (fd = Checkwrite(line, &statb, O_RDWR, 1, 0 )) < 0) {
		err = errno;
		FPRINTF( STDERR,
			"open '%s' failed: wrong result - '%s'\n",
			line, Errormsg(errno)  );
		exit(1);
	}
	if( Do_lock( fd, 0 ) < 0 ) {
		FPRINTF( STDERR,
			"Mother could not lock '%s', in correct result\n", line );
		exit(0);
	}
	plp_snprintf( cmd, sizeof(cmd), "ls -l %s", line );
	i = system( cmd );
	if( (pid = fork()) < 0 ){
		FPRINTF( STDERR, "fork failed!\n");
	} else if ( pid == 0 ){
		FPRINTF( STDERR, "Daughter re-opening and locking '%s'\n", line );
		close( fd );
		if( (fd = Checkwrite(line, &statb, O_RDWR, 1, 0 )) < 0) {
			err = errno;
			FPRINTF( STDERR,
				"Daughter re-open '%s' failed: wrong result - '%s'\n",
				line, Errormsg(errno)  );
			exit(1);
		}
		if( Do_lock( fd, 0 ) < 0 ) {
			FPRINTF( STDERR,
				"Daughter could not lock '%s', correct result\n", line );
			exit(0);
		}
		FPRINTF( STDERR,
			"Daughter locked '%s', incorrect result\n", line );
		exit(1);
	}
	plp_usleep(1000);
	status = 0;
	while(1){
		result = plp_waitpid( -1, &status, 0 );
		if( result == pid ){
			FPRINTF( STDERR, "Daughter exit status %d\n", status );
			break;
		} else if( (result == -1 && errno == ECHILD) || result == 0 ){
			break;
		} else if( result == -1 && errno != EINTR ){
			FPRINTF( STDERR,
				"plp_waitpid() failed!  This should not happen!");
			status = -1;
			break;
		}
	}
	if( status == 0 ){
		FPRINTF( STDERR, "***** Lockf() works\n" );
	}

	if( (pid = fork()) < 0 ){
		FPRINTF( STDERR, "fork failed!\n");
	} else if ( pid == 0 ){
		int lock = 0;
		FPRINTF( STDERR, "Daughter re-opening '%s'\n", line );
		close( fd );
		if( (fd = Checkwrite(line, &statb, O_RDWR, 1, 0 )) < 0) {
			err = errno;
			FPRINTF( STDERR,
				"Daughter re-open '%s' failed: wrong result - '%s'\n",
				line, Errormsg(errno)  );
			exit(1);
		}
		FPRINTF( STDERR, "Daughter blocking for lock\n" );
		lock = Do_lock( fd, 1 );
		if( lock < 0 ){
			FPRINTF( STDERR, "Daughter lock '%s' failed! wrong result\n",
				line );
			exit( 1 );
		}
		FPRINTF( STDERR, "Daughter lock '%s' succeeded, correct result\n",
			line );
		exit(0);
	}
	FPRINTF( STDERR, "Mother pausing before releasing lock on fd %d\n", fd );
	plp_sleep(3);

	FPRINTF( STDERR, "Mother closing '%s', releasing lock on fd %d\n",
		line, fd );
	close( fd );
	fd = -1;
	status = 0;
	while(1){
		result = plp_waitpid( -1, &status, 0 );
		if( result == pid ){
			FPRINTF( STDERR, "Daughter exit status %d\n", status );
			break;
		} else if( (result == -1 && errno == ECHILD) || result == 0 ){
			break;
		} else if( result == -1 && errno != EINTR ){
			FPRINTF( STDERR,
				"plp_waitpid() failed!  This should not happen!");
			status = -1;
			break;
		}
	}
	if( status == 0 ){
		FPRINTF( STDOUT, "***** Lockf() with unlocking works\n" );
	}

	if( fd >= 0 ) close(fd);
	fd = - 1;
	unlink( line );


/***************************************************************************
 * check out the process title
 ***************************************************************************/

	FPRINTF( STDOUT, "checking if setting process info to 'lpd XXYYZZ' works\n" );
	setproctitle( "lpd %s", "XXYYZZ" );
	/* try simple test first */
	i = 0;
	if( (tf = popen( "ps | grep XXYYZZ | grep -v grep", "r" )) ){
		Max_open( fileno(tf) );
		while( fgets( line, sizeof(line), tf ) ){
			FPRINTF( STDOUT, "%s", line );
			++i;
		}
		fclose(tf);
	}
	
	if( i == 0 && (tf = popen( "ps | grep XXYYZZ | grep -v grep", "r" )) ){
		Max_open( fileno(tf) );
		while( fgets( line, sizeof(line), tf ) ){
			FPRINTF( STDOUT, "%s", line );
			++i;
		}
		fclose(tf);
	}
	if( i ){
		FPRINTF( STDOUT, "***** setproctitle works\n" );
	} else {
		FPRINTF( STDOUT, "***** setproctitle debugging aid unavailable (not a problem)\n" );
	}
	exit(0);
}

void Fix_clean( char *s, int no )
{
	struct stat statb;
	int fd;
	if( s ){
		if(!no){
			Make_write_file( s, 0 );
			if( Truncate >= 0 ){
				MESSAGE(" trimming '%s'", s );
				fd = Trim_status_file( -1, s, Truncate, Truncate );
				close(fd);
			}
		} else {
			if( stat(s,&statb) == 0 && Fix ){
				MESSAGE(" removing '%s'", s );
				unlink(s);
			}
		}
	}
}

int Check_path_list( char *plist, int allow_missing )
{
	struct line_list values;
	char *path;
	int found_pc = 0, i, fd;
	struct stat statb;

	Init_line_list(&values);
	Split(&values,plist,File_sep,0,0,0,0,0,0);
	for( i = 0; i < values.count; ++i ){
		path = values.list[i];
		if( path[0] == '|' ){
			++path;
			Check_executable_filter( path, 0 );
		} else if( path[0] == '/' ){
			if( (fd = Checkread( path,&statb ) ) < 0 ){
				if( stat( path, &statb ) ){
					if( ! allow_missing ) WARNMSG(" '%s' not present", path );
				} else {
					WARNMSG(" '%s' cannot be opened - check path permissions", path );
				}
			} else {
				close(fd);
				if(Verbose)MESSAGE("  found '%s', mod 0%o", path, (unsigned int)statb.st_mode );
				++found_pc;
				if( (statb.st_mode & 0444) != 0444 ){
					WARNMSG(" '%s' is not world readable", path );
					WARNMSG(" this file should have (suggested) 644 permissions, owned by root" );
				}
			}
		} else {
			WARNMSG("not absolute pathname '%s' in '%s'", path, plist );
		}
	}
	Free_line_list(&values);
	return(found_pc);
}
