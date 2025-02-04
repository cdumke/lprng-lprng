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
#include "gethostinfo.h"
#include "proctitle.h"
#include "getprinter.h"
#include "getqueue.h"
#include "child.h"
#include "fileopen.h"
#include "sendreq.h"
#include "globmatch.h"
#include "permission.h"
#include "lockfile.h"
#include "errorcodes.h"

#include "lpd_jobs.h"
#include "lpd_status.h"

/**** ENDINCLUDE ****/

/***************************************************************************
 * Commentary:
 * Patrick Powell Tue May  2 09:32:50 PDT 1995
 * 
 * Return status:
 * 	status has two formats: short and long
 * 
 * Status information is obtained from 3 places:
 * 1. The status file
 * 2. any additional progress files indicated in the status file
 * 3. job queue
 * 
 * The status file is maintained by the current unspooler process.
 * It updates this file with the following information:
 * 
 * PID of the unspooler process   [line 1]
 * active job and  status file name
 * active job and  status file name
 * 
 * Example 1:
 * 3012
 * cfa1024host status
 * 
 * Example 2:
 * 3015
 * cfa1024host statusfd2
 * cfa1026host statusfd1
 * 
 * Format of the information reporting:
 * 
 * 0        1         2         3         4         5         6         7
 * 12345678901234567890123456789012345678901234567890123456789012345678901234
 *  Rank   Owner/ID                   Class Job  Files               Size Time
 * 1      papowell@astart4+322          A  322 /tmp/hi                3 17:33:47
 * x     Sx                           SxSx    Sx                 Sx    Sx       X
 *                                                                              
 ***************************************************************************/

#define RANKW 7
#define OWNERW 29
#define CLASSW 2
#define JOBW 6
#define FILEW 18
#define SIZEW 6
#define TIMEW 8

static void Print_status_info( int *sock, char *file,
	char *prefix, int status_lines, int max_size );

int Job_status( int *sock, char *input )
{
	char *s, *t, *name, *hash_key;
	int displayformat, status_lines = 0, i, n;
	struct line_list l, listv;
	struct line_list done_list;
	char error[SMALLBUFFER], buffer[16];
	int db, dbflag;

	Init_line_list(&l);
	Init_line_list(&listv);
	Init_line_list(&done_list);
	db = Debug;
	dbflag = DbgFlag;

	Name = "Job_status";

	/* get the format */
	if( (s = safestrchr(input, '\n' )) ) *s = 0;
	displayformat = *input++;

	/*
	 * if we get a short/long request from these hosts,
	 * reverse the sense of question
	 */
	if( Reverse_lpq_status_DYN
		&& (displayformat == REQ_DSHORT || displayformat==REQ_DLONG)  ){
		Free_line_list(&l);
		Split(&l,Reverse_lpq_status_DYN,File_sep,0,0,0,0,0,0);
		if( Match_ipaddr_value( &l, &RemoteHost_IP ) == 0 ){
			DEBUGF(DLPQ1)("Job_status: reversing status sense");
			if( displayformat == REQ_DSHORT ){
				displayformat = REQ_DLONG;
			} else {
				displayformat = REQ_DSHORT;
			}
		}
		Free_line_list(&l);
	}
	/*
	 * we have a list of hosts with format of the form:
	 *  Key=list; Key=list;...
	 *  key is s for short, l for long
	 */
	DEBUGF(DLPQ1)("Job_status: Force_lpq_status_DYN '%s'", Force_lpq_status_DYN);
	if( Force_lpq_status_DYN ){
		Free_line_list(&listv);
		Split(&listv,Force_lpq_status_DYN,";",0,0,0,0,0,0);
		for(i = 0; i < listv.count; ++i ){
			s = listv.list[i];
			if( (t = safestrpbrk(s,Hash_value_sep)) ) *t++ = 0;
			Free_line_list(&l);
			Split(&l,t,File_sep,0,0,0,0,0,0);
			DEBUGF(DLPQ1)("Job_status: Force_lpq_status '%s'='%s'", s,t);
			if( Match_ipaddr_value( &l, &RemoteHost_IP ) == 0 ){
				DEBUGF(DLPQ1)("Job_status: forcing status '%s'", s);
				if( safestrcasecmp(s,"s") == 0 ){
					displayformat = REQ_DSHORT;
				} else if( safestrcasecmp(s,"l") == 0 ){
					displayformat = REQ_DLONG;
				}
				status_lines = Short_status_length_DYN;
				break;
			}
		}
		Free_line_list(&l);
		Free_line_list(&listv);
	}

	/*
	 * check for short status to be returned
	 */

	if( Return_short_status_DYN && displayformat == REQ_DLONG ){
		Free_line_list(&l);
		Split(&l,Return_short_status_DYN,File_sep,0,0,0,0,0,0);
		if( Match_ipaddr_value( &l, &RemoteHost_IP ) == 0 ){
			status_lines = Short_status_length_DYN;
			DEBUGF(DLPQ1)("Job_status: truncating status to %d",
				status_lines);
		}
		Free_line_list(&l);
	}

	DEBUGF(DLPQ1)("Job_status: doing '%s'", input );
	Free_line_list(&l);
	Split(&l,input,Whitespace,0,0,0,0,0,0);
	if( l.count == 0 ){
		plp_snprintf( error, sizeof(error), "zero length command line");
		goto error;
	}

	/* save l.list[0] */
	name = l.list[0];
	
	if( (s = Is_clean_name( name )) ){
		plp_snprintf( error, sizeof(error),
			_("printer '%s' has illegal character at '%s' in name"), name, s );
		goto error;
	}

	Set_DYN(&Printer_DYN,name);
	setproctitle( "lpd %s '%s'", Name, name );
	plp_snprintf(buffer,sizeof(buffer), "%d",displayformat);
	l.list[0] = buffer;

	/* we have the hash key */
	hash_key = Join_line_list_with_sep(&l,"_");
	for( s = hash_key; (s = strpbrk(s,Whitespace)); ) *s = '_';
	
	DEBUGF(DLPQ1)("Job_status: arg '%s'", s );
	/* now we put back the l.list[0] value */

	l.list[0] = name;
	/* free the values l.list[0] */
	Remove_line_list( &l, 0 );
	name = Printer_DYN;

	if( l.count && (s = l.list[0]) && s[0] == '-' ){
		DEBUGF(DLPQ1)("Job_status: arg '%s'", s );
		Free_line_list(&listv);
		Split(&listv,s+1,Arg_sep,1,Hash_value_sep,1,1,0,0);
		Remove_line_list( &l, 0 );
		DEBUGFC(DLPQ1)Dump_line_list( "Job_status: args", &listv );
		if( (n = Find_flag_value(&listv,"lines")) ) status_lines = n;
		DEBUGF(DLPQ1)("Job_status: status_lines '%d'", status_lines );
		Free_line_list(&listv);
	}
	if( safestrcasecmp( name, ALL ) ){
		DEBUGF(DLPQ1)("Job_status: checking printcap entry '%s'",  name );
		Get_queue_status( &l, sock, displayformat, status_lines,
			&done_list, Max_status_size_DYN, hash_key );
	} else {
		/* we work our way down the printcap list, checking for
			ones that have a spool queue */
		/* note that we have already tried to get the 'all' list */
		
		Get_all_printcap_entries();
		for( i = 0; i < All_line_list.count; ++i ){
			Set_DYN(&Printer_DYN, All_line_list.list[i] );
			Debug = db;
			DbgFlag = dbflag;
			Get_queue_status( &l, sock, displayformat, status_lines,
				&done_list, Max_status_size_DYN, hash_key );
		}
	}
	Free_line_list( &l );
	Free_line_list( &listv );
	Free_line_list( &done_list );
	DEBUGF(DLPQ3)("Job_status: DONE" );
	return(0);

 error:
	DEBUGF(DLPQ2)("Job_status: error msg '%s'", error );
	i = safestrlen(error);
	if( (i = safestrlen(error)) >= (int)sizeof(error)-2 ){
		i = sizeof(error) - 2;
	}
	error[i++] = '\n';
	error[i] = 0;
	Free_line_list( &l );
	Free_line_list( &listv );
	Free_line_list( &done_list );
	if( Write_fd_str( *sock, error ) < 0 ) cleanup(0);
	DEBUGF(DLPQ3)("Job_status: done" );
	return(0);
}

/***************************************************************************
 * void Get_queue_status
 * sock - used to send information
 * displayformat - REQ_DSHORT, REQ_DLONG, REQ_VERBOSE
 * tokens - arguments
 *  - get the printcap entry (if any)
 *  - check the control file for current status
 *  - find and report the spool queue entries
 ***************************************************************************/
void Get_queue_status( struct line_list *tokens, int *sock,
	int displayformat, int status_lines, struct line_list *done_list,
	int max_size, char *hash_key )
{
	char msg[SMALLBUFFER], buffer[SMALLBUFFER], error[SMALLBUFFER],
		number[LINEBUFFER], header[LARGEBUFFER];
	char sizestr[SIZEW+TIMEW+32];
	const char *identifier, *cs;
	char *pr, *s, *t, *path,
		*jobname, *joberror, *class, *priority, *d_identifier,
		*job_time, *d_error, *d_dest, *cftransfername, *hf_name, *filenames,
		*tempfile = 0, *file = 0, *end_of_name;
	struct line_list outbuf, info, lineinfo, cache, cache_info;
	int status = 0, len, ix, nx, flag, count, held, move,
		server_pid, unspooler_pid, fd, nodest,
		printable, dcount, destinations = 0,
		d_copies, d_copy_done, permission, jobnumber, db, dbflag,
		matches, tempfd, savedfd, lockfd, delta, err, cache_index,
		total_held, total_move, jerror, jdone;
	double jobsize;
	struct stat statb;
	struct job job;
	time_t modified = 0;
	time_t timestamp = 0;
	time_t now = time( (void *)0 );

	cache_index = -1;

	DEBUG1("Get_queue_status: sock fd %d, checking '%s'", *sock, Printer_DYN );
	if(DEBUGL1)Dump_line_list( "Get_queue_status: done_list", done_list );

	/* set printer name and printcap variables */

	Init_job(&job);
	Init_line_list(&info);
	Init_line_list(&lineinfo);
	Init_line_list(&outbuf);
	Init_line_list(&cache);
	Init_line_list(&cache_info);
	/* for caching */
	tempfile = 0; 
	savedfd = tempfd = lockfd = -1;

	Check_max(tokens,2);
	tokens->list[tokens->count] = 0;
	msg[0] = 0;
	header[0] = 0;
	error[0] = 0;
	pr = 0; s = 0;

	safestrncpy(buffer,Printer_DYN);
	status = Setup_printer( Printer_DYN, error, sizeof(error), 0);
	if( status ){
		if( error[0] == 0 ){
			plp_snprintf(error,sizeof(error), "Nonexistent printer '%s'", Printer_DYN);
		}
		goto error;
	}

	db = Debug;
	dbflag = DbgFlag;
	s = Find_str_value(&Spool_control,DEBUG);
	if( !s ) s = New_debug_DYN;
	Parse_debug( s, 0 );
	if( !(DLPQMASK & DbgFlag) ){
		Debug = db;
		DbgFlag = dbflag;
	} else {
		int odb, odbf;
		odb = Debug;
		odbf = DbgFlag;
		Debug = db;
		DbgFlag = dbflag;
		if( Log_file_DYN ){
			fd = Trim_status_file( -1, Log_file_DYN, Max_log_file_size_DYN,
				Min_log_file_size_DYN );
			if( fd > 0 && fd != 2 ){
				dup2(fd,2);
				close(fd);
				close(fd);
			}
		}
		Debug = odb;
		DbgFlag = odbf;
	}

	DEBUGF(DLPQ3)("Get_queue_status: sock fd %d, Setup_printer status %d '%s'", *sock, status, error );
	/* set up status */
	if( Find_exists_value(done_list,Printer_DYN,Hash_value_sep ) ){
		return;
	}
	Add_line_list(done_list,Printer_DYN,Hash_value_sep,1,1);

	/* check for permissions */

	Perm_check.service = 'Q';
	Perm_check.printer = Printer_DYN;

	permission = Perms_check( &Perm_line_list, &Perm_check, 0, 0 );
	DEBUGF(DLPQ1)("Job_status: permission '%s'", perm_str(permission));
	if( permission == P_REJECT ){
		plp_snprintf( error, sizeof(error),
			_("%s: no permission to show status"), Printer_DYN );
		goto error;
	}

	/* check to see if we have any cached information */
	if( Lpq_status_cached_DYN > 0 && Lpq_status_file_DYN ){
		fd = -1;
		do{
			DEBUGF(DLPQ1)("Job_status: getting lock on '%s'", Lpq_status_file_DYN);
			lockfd = Checkwrite( Lpq_status_file_DYN, &statb, O_RDWR, 1, 0 );
			if( lockfd < 0 ){
				logerr_die(LOG_INFO, "Get_queue_status: cannot open '%s'",
				Lpq_status_file_DYN);
			}
			if( Do_lock(lockfd, 0) < 0 ){
				DEBUGF(DLPQ1)("Get_queue_status: did not get lock");
				Do_lock(lockfd, 1);
				DEBUGF(DLPQ1)("Get_queue_status: lock released");
				close(lockfd); lockfd = -1;
			}
		}while( lockfd < 0 );
		DEBUGF(DLPQ1)("Get_queue_status: lock succeeded");
		Free_line_list(&cache);
		Get_fd_image_and_split(lockfd, 0,0,&cache,Line_ends,0,0,0,0,0,0);
		DEBUGFC(DLPQ3)Dump_line_list("Get_queue_status- cache", &cache );
		DEBUGF(DLPQ3)("Get_queue_status: cache hash_key '%s'", hash_key );
		file = 0;
		nx = -1;
		if( cache.count < Lpq_status_cached_DYN ){
			Check_max(&cache,Lpq_status_cached_DYN-cache.count);
			for( ix = cache.count; ix < Lpq_status_cached_DYN; ++ix ){
				cache.list[ix] = 0;
			}
			cache.count = ix;
		}
		for( ix = 0; file == 0 && ix < cache.count; ++ix ){
			if( (s = cache.list[ix]) ){
				if( (t = safestrchr(s,'=')) ){
					*t = 0;
					if( !strcmp(hash_key,s) ){
						file = t+1;
						cache_index = ix;
					}
					*t = '=';
				}
			}
		}
		/* if we have a file name AND it is not stale then we use it */
		DEBUGF(DLPQ3)("Get_queue_status: found in cache '%s'", file );
		fd = -1;
		if( file ){
			Split(&cache_info,file,Arg_sep,1,Hash_value_sep,1,1,0,0);
			file = Find_str_value(&cache_info,FILENAMES);
		}
		DEBUGFC(DLPQ3)Dump_line_list("Get_queue_status: cache_info", &cache_info );
		if( file && (fd = Checkread( file, &statb )) > 0 ){
			modified = statb.st_mtime;
			delta = now - modified;
			if( Lpq_status_stale_DYN && delta > Lpq_status_stale_DYN ){
				/* we cannot use it */
				close(fd); fd = -1;
			}  
		}
		if( fd > 0 ){
			modified = 0;
			if( Queue_status_file_DYN && stat(Queue_status_file_DYN,&statb) == 0 ){
				modified = statb.st_mtime;
			}
			timestamp = Find_flag_value(&cache_info,QUEUE_STATUS_FILE);
			delta = modified - timestamp;
			DEBUGF(DLPQ3)("Get_queue_status: queue status '%s', modified %lx, timestamp %lx, delta %d",
				Queue_status_file_DYN, (long)(modified), (long)(timestamp), delta );
			if( delta > Lpq_status_interval_DYN ){
				/* we need to refresh the data */
				close(fd); fd = -1;
			}
		}

		if( fd > 0 ){
			if( Status_file_DYN && stat(Status_file_DYN,&statb) == 0 ){
				modified = statb.st_mtime;
			}
			timestamp = Find_flag_value(&cache_info,PRSTATUS);
			delta = modified - timestamp;
			DEBUGF(DLPQ3)("Get_queue_status: pr status '%s', modified %lx, timestamp %lx, delta %d",
				Status_file_DYN, (long)(modified), (long)(timestamp), delta );
			if( delta > Lpq_status_interval_DYN ){
				/* we need to refresh the data */
				close(fd); fd = -1;
			}
		}

		if( fd > 0 ){
			DEBUGF(DLPQ3)("Get_queue_status: reading cached status from fd '%d'", fd );
			/* We can read the status from the cached data */
			while( (ix = ok_read( fd, buffer, sizeof(buffer)-1 )) > 0 ){
				if( write( *sock, buffer, ix ) < 0 ){
					cleanup(0);
				}
			}
			close(fd); fd = -1;
			goto remote;
		}
		/* OK, we have to cache the status in a file */
		tempfd = Make_temp_fd( &tempfile );
		savedfd = *sock;
		*sock = tempfd;
	}

	end_of_name = 0;
	if( displayformat != REQ_DSHORT ){
		plp_snprintf( header, sizeof(header), "%s: ",
			Server_queue_name_DYN?"Server Printer":"Printer" );
	}
	len = strlen(header);
	plp_snprintf( header+len, sizeof(header)-len, "%s@%s",
		Printer_DYN, Report_server_as_DYN?Report_server_as_DYN:ShortHost_FQDN );
	if( safestrcasecmp( buffer, Printer_DYN ) ){
		len = strlen(header);
		plp_snprintf( header+len, sizeof(header)-len, _(" (originally %s)"), buffer );
	}
	end_of_name = header+strlen(header);

/* TODO: gcc complains that is never looked at. And indeed it looks like
 * status is checked above and it does not end up here.
 * Why is this code here? - brl */
	if( status ){
		len = strlen( header );
		if( displayformat == REQ_VERBOSE ){
			safestrncat( header, _("\n Error: ") );
			len = strlen( header );
		}
		if( error[0] ){
			plp_snprintf( header+len, sizeof(header)-len,
				_(" - %s"), error );
		} else if( !Spool_dir_DYN ){
			plp_snprintf( header+len, sizeof(header)-len,
				_(" - printer %s@%s not in printcap"), Printer_DYN,
					Report_server_as_DYN?Report_server_as_DYN:ShortHost_FQDN );
		} else {
			plp_snprintf( header+len, sizeof(header)-len,
				_(" - printer %s@%s has bad printcap entry"), Printer_DYN,
					Report_server_as_DYN?Report_server_as_DYN:ShortHost_FQDN );
		}
		safestrncat( header, "\n" );
		DEBUGF(DLPQ3)("Get_queue_status: forward header '%s'", header );
		if( Write_fd_str( *sock, header ) < 0 ) cleanup(0);
		header[0] = 0;
		goto done;
	}
	if( displayformat == REQ_VERBOSE ){
		safestrncat( header, "\n" );
		if( Write_fd_str( *sock, header ) < 0 ) cleanup(0);
		header[0] = 0;
	}

	/* get the spool entries */
	Free_line_list( &outbuf );
	Scan_queue( &Spool_control, &Sort_order, &printable,&held,&move,0,0,0,0,0 );
	/* check for done jobs, remove any if there are some */
	if( Remove_done_jobs() ){
		Scan_queue( &Spool_control, &Sort_order, &printable,&held,&move,0,0,0,0,0 );
	}

	DEBUGF(DLPQ3)("Get_queue_status: total files %d", Sort_order.count );
	DEBUGFC(DLPQ3)Dump_line_list("Get_queue_status- Sort_order", &Sort_order );


	/* set up the short format for folks */

	if( displayformat == REQ_DLONG && Sort_order.count > 0 ){
		/*
		 Rank  Owner/ID  Class Job Files   Size Time
		*/
		Add_line_list(&outbuf,
" Rank   Owner/ID               Pr/Class Job Files                 Size Time"
		,0,0,0);
	}
	error[0] = 0;

	matches = 0;
	total_held = 0;
	total_move = 0;
	for( count = 0; count < Sort_order.count; ++count ){
		int printable, held, move;
		char prclass[32];
		printable = held = move = 0;
		Free_job(&job);
		Get_job_ticket_file( 0, &job, Sort_order.list[count] );
		if( job.info.count == 0 ){
			/* job was removed */
			continue;
		}
		Job_printable(&job,&Spool_control, &printable,&held,&move,&jerror,&jdone);
		DEBUGF(DLPQ3)("Get_queue_status: printable %d, held %d, move %d, error %d, done %d",
			printable, held, move, jerror, jdone );
		DEBUGFC(DLPQ4)Dump_job("Get_queue_status - info", &job );
		if( job.info.count == 0 ) continue;

		if( tokens->count && Patselect( tokens, &job.info, 0) ){
			continue;
		}

		number[0] = 0;
		error[0] = 0;
		msg[0] = 0;
		nodest = 0;
		s = Find_str_value(&job.info,PRSTATUS);
		if( s == 0 ){
			plp_snprintf(number,sizeof(number), "%d",count+1);
		} else {
			plp_snprintf(number,sizeof(number), "%s",s);
		}
		identifier = Find_str_value(&job.info,IDENTIFIER);
		if( identifier == 0 ){
			identifier = Find_str_value(&job.info,LOGNAME);
		}
		if( identifier == 0 ){
			identifier = "???";
		}
		priority = Find_str_value(&job.info,PRIORITY);
		class = Find_str_value(&job.info,CLASS);
		jobname = Find_str_value(&job.info,JOBNAME);
		filenames = Find_str_value(&job.info,FILENAMES);
		jobnumber = Find_decimal_value(&job.info,NUMBER);
		joberror = Find_str_value(&job.info,ERROR);
		jobsize = Find_double_value(&job.info,SIZE);
		job_time = Find_str_value(&job.info,JOB_TIME );
		destinations = Find_flag_value(&job.info,DESTINATIONS);
		cftransfername = Find_str_value(&job.info,XXCFTRANSFERNAME);
		hf_name = Find_str_value(&job.info,HF_NAME);

		/* we report this jobs status */

		DEBUGF(DLPQ3)("Get_queue_status: joberror '%s'", joberror );
		DEBUGF(DLPQ3)("Get_queue_status: class '%s', priority '%s'",
			class, priority );

		if( class ){
			if( safestrcmp(class,priority)
				|| Class_in_status_DYN || priority == 0 ){
				plp_snprintf( prclass, sizeof(prclass), "%s/%s",
					priority?priority:"?", class );
				priority = prclass;
			}
		}

		if( displayformat == REQ_DLONG ){
			plp_snprintf( msg, sizeof(msg),
				"%-*s %-*s ", RANKW-1, number, OWNERW-1, identifier );
			while( (len = safestrlen(msg)) > (RANKW+OWNERW)
				&& isspace(cval(msg+len-1)) && isspace(cval(msg+len-2)) ){
				msg[len-1] = 0;
			}
			plp_snprintf( buffer, sizeof(buffer), "%-*s %*d ",
				CLASSW-1,priority, JOBW-1,jobnumber);
			DEBUGF(DLPQ3)("Get_queue_status: msg len %d '%s', buffer %d, '%s'",
				safestrlen(msg),msg, safestrlen(buffer), buffer );
			DEBUGF(DLPQ3)("Get_queue_status: RANKW %d, OWNERW %d, CLASSW %d, JOBW %d",
				RANKW, OWNERW, CLASSW, JOBW );
			s = buffer;
			while( safestrlen(buffer) > CLASSW+JOBW && (s = safestrchr(s,' ')) ){
				if( cval(s+1) == ' ' ){
					memmove(s,s+1,safestrlen(s)+1);
				} else {
					++s;
				}
			}
			s = msg+safestrlen(msg)-1;
			while( safestrlen(msg) + safestrlen(buffer) > RANKW+OWNERW+CLASSW+JOBW ){
				if( cval(s) == ' ' && cval(s-1) == ' ' ){
					*s-- = 0;
				} else {
					break;
				}
			}
			s = buffer;
			while( safestrlen(msg) + safestrlen(buffer) > RANKW+OWNERW+CLASSW+JOBW
				&& (s = safestrchr(s,' ')) ){
				if( cval(s+1) == ' ' ){
					memmove(s,s+1,safestrlen(s)+1);
				} else {
					++s;
				}
			}
			len = safestrlen(msg);

			plp_snprintf(msg+len, sizeof(msg)-len, "%s",buffer);
			if( joberror ){
				len = safestrlen(msg);
					plp_snprintf(msg+len,sizeof(msg)-len,
					"ERROR: %s", joberror );
			} else {
				char jobb[32];
				DEBUGF(DLPQ3)("Get_queue_status: jobname '%s'", jobname );

				len = safestrlen(msg);
				plp_snprintf(msg+len,sizeof(msg)-len, "%-s",jobname?jobname:filenames);
				plp_snprintf(jobb,sizeof(jobb), "%0.0f", jobsize );

				job_time = Time_str(1, Convert_to_time_t(job_time));
				if( !Full_time_DYN && (t = safestrchr(job_time,'.')) ) *t = 0;
				plp_snprintf( sizestr, sizeof(sizestr), "%*s %-s",
					SIZEW-1,jobb, job_time );
				DEBUGF(DLPQ3)("XGet_queue_status: size_str '%s'",sizestr);

				len = Max_status_line_DYN;
				if( len >= (int)sizeof(msg)) len = sizeof(msg)-1;
				len = len-safestrlen(sizestr);
				if( len > 0 ){
					/* pad with spaces */
					for( nx = safestrlen(msg); nx < len; ++nx ){
						msg[nx] = ' ';
					}
					msg[nx] = 0;
				}
				/* remove spaces if necessary */
				while( safestrlen(msg) + safestrlen(sizestr) > Max_status_line_DYN ){
					if( isspace( cval(sizestr) ) ){
						memmove(sizestr, sizestr+1, safestrlen(sizestr)+1);
					} else {
						s = msg+safestrlen(msg)-1;
						if( isspace(cval(s)) && isspace(cval(s-1)) ){
							s[0] = 0;
						} else {
							break;
						}
					}
				}
				if( safestrlen(msg) + safestrlen(sizestr) >= Max_status_line_DYN ){
					len = Max_status_line_DYN - safestrlen(sizestr);
					msg[len-1] = ' ';
					msg[len] = 0;
				}
				strcpy( msg+safestrlen(msg), sizestr );
			}

			if( Max_status_line_DYN < (int)sizeof(msg) ) msg[Max_status_line_DYN] = 0;

			DEBUGF(DLPQ3)("Get_queue_status: adding '%s'", msg );
			Add_line_list(&outbuf,msg,0,0,0);
			DEBUGF(DLPQ3)("Get_queue_status: destinations '%d'", destinations );
			if( nodest == 0 && destinations ){
				for( dcount = 0; dcount < destinations; ++dcount ){
					if( Get_destination( &job, dcount ) ) continue;
					DEBUGFC(DLPQ3)Dump_line_list("Get_queue_status: destination",
						&job.destination);
					d_error =
						Find_str_value(&job.destination,ERROR);
					d_dest =
						Find_str_value(&job.destination,DEST);
					d_copies = 
						Find_flag_value(&job.destination,COPIES);
					d_copy_done = 
						Find_flag_value(&job.destination,COPY_DONE);
					d_identifier =
						Find_str_value(&job.destination,IDENTIFIER);
					cs = Find_str_value(&job.destination, PRSTATUS);
					if( !cs ) cs = "";
					plp_snprintf(number, sizeof(number), " - %-8s", cs );
					plp_snprintf( msg, sizeof(msg),
						"%-*s %-*s ", RANKW, number, OWNERW, d_identifier );
					len = safestrlen(msg);
					plp_snprintf(msg+len, sizeof(msg)-len, " ->%s", d_dest );
					if( d_copies > 1 ){
						len = safestrlen( msg );
						plp_snprintf( msg+len, sizeof(msg)-len,
							_(" <cpy %d/%d>"), d_copy_done, d_copies );
					}
					if( d_error ){
						len = safestrlen(msg);
						plp_snprintf( msg+len, sizeof(msg)-len, " ERROR: %s", d_error );
					}
					Add_line_list(&outbuf,msg,0,0,0);
				}
			}
			DEBUGF(DLPQ3)("Get_queue_status: after dests" );
		} else if( displayformat == REQ_VERBOSE ){
			plp_snprintf( header, sizeof(header),
				_(" Job: %s"), identifier );
			plp_snprintf( msg, sizeof(msg), _("%s status= %s"),
				header, number );
			Add_line_list(&outbuf,msg,0,0,0);
			plp_snprintf( msg, sizeof(msg), _("%s size= %0.0f"),
				header, jobsize );
			Add_line_list(&outbuf,msg,0,0,0);
			plp_snprintf( msg, sizeof(msg), _("%s time= %s"),
				header, job_time );
			Add_line_list(&outbuf,msg,0,0,0);
			if( joberror ){
				plp_snprintf( msg, sizeof(msg), _("%s error= %s"),
						header, joberror );
				Add_line_list(&outbuf,msg,0,0,0);
			}
			if( cftransfername ){
				plp_snprintf( msg, sizeof(msg), _("%s CONTROL="), header );
				Add_line_list(&outbuf,msg,0,0,0);
				s = Find_str_value(&job.info,CF_OUT_IMAGE);
				Add_line_list(&outbuf,s,0,0,0);
			}

			plp_snprintf( msg, sizeof(msg), _("%s HOLDFILE="), header );
			Add_line_list(&outbuf,msg,0,0,0);
			s = Make_job_ticket_image(&job);
			Add_line_list(&outbuf,s,0,0,0);
			free(s); s = NULL;
		} else if( displayformat == REQ_DSHORT ){
			if( printable ){
				++matches;
			} else if( held ){
				++total_held;
			} else if( move ){
				++total_move;
			}
		}
	}
	DEBUGF(DLPQ3)("Get_queue_status: matches %d", matches );
	/* this gives a short 1 line format with minimum info */
	if( displayformat == REQ_DSHORT ){
		len = safestrlen( header );
		plp_snprintf( header+len, sizeof(header)-len,
				ngettext(" %d job", " %d jobs", matches),
				matches);
		if( total_held ){
			len = safestrlen( header );
			plp_snprintf( header+len, sizeof(header)-len, _(" (%d held)"),
				total_held );
		}
		if( total_move ){
			len = safestrlen( header );
			plp_snprintf( header+len, sizeof(header)-len, _(" (%d move)"),
				total_move );
		}
	}
	len = safestrlen( header );

	DEBUGFC(DLPQ4)Dump_line_list("Get_queue_status: job status",&outbuf);

	DEBUGF(DLPQ3)(
		"Get_queue_status: RemoteHost_DYN '%s', RemotePrinter_DYN '%s', Lp '%s'",
		RemoteHost_DYN, RemotePrinter_DYN, Lp_device_DYN );

	if( displayformat != REQ_DSHORT ){
		s = 0;
		if( (s = Comment_tag_DYN) == 0 ){
			if( (nx = PC_alias_line_list.count) > 1 ){
				s = PC_alias_line_list.list[nx-1];
			}
		}
		if( s ){
			s = Fix_str(s);
			len = safestrlen( header );
			if( displayformat == REQ_VERBOSE ){
				plp_snprintf( header+len, sizeof(header)-len, _(" Comment: %s"), s );
			} else {
				plp_snprintf( header+len, sizeof(header)-len, " '%s'", s );
			}
			free(s); s = NULL;
		}
	}

	len = safestrlen( header );
	if( displayformat == REQ_VERBOSE ){
		plp_snprintf( header+len, sizeof(header)-len,
			_("\n Printing: %s\n Aborted: %s\n Spooling: %s"),
				Pr_disabled(&Spool_control)?"yes":"no",
				Pr_aborted(&Spool_control)?"yes":"no",
				Sp_disabled(&Spool_control)?"yes":"no");
	} else if( displayformat == REQ_DLONG || displayformat == REQ_DSHORT ){
		flag = 0;
		if( Pr_disabled(&Spool_control) || Sp_disabled(&Spool_control) || Pr_aborted(&Spool_control) ){
			plp_snprintf( header+len, sizeof(header)-len, " (" );
			len = safestrlen( header );
			if( Pr_disabled(&Spool_control) ){
				plp_snprintf( header+len, sizeof(header)-len, "%s%s",
					flag?", ":"", "printing disabled" );
				flag = 1;
				len = safestrlen( header );
			}
			if( Pr_aborted(&Spool_control) ){
				plp_snprintf( header+len, sizeof(header)-len, "%s%s",
					flag?", ":"", "printing aborted" );
				flag = 1;
				len = safestrlen( header );
			}
			if( Sp_disabled(&Spool_control) ){
				plp_snprintf( header+len, sizeof(header)-len, "%s%s",
					flag?", ":"", "spooling disabled" );
				len = safestrlen( header );
			}
			plp_snprintf( header+len, sizeof(header)-len, ")" );
			len = safestrlen( header );
		}
	}

	/*
	 * check to see if this is a server or subserver.  If it is
	 * for subserver,  then you can forget starting it up unless started
	 * by the server.
	 */
	if( (s = Server_names_DYN) || (s = Destinations_DYN) ){
		Split( &info, s, File_sep, 0,0,0,0,0,0);
		len = safestrlen( header );
		if( displayformat == REQ_VERBOSE ){
			if ( Server_names_DYN ) {
				cs = "Subservers";
			} else {
				cs = "Destinations";
			}
			plp_snprintf( header+len, sizeof(header)-len,
			_("\n %s: "), cs );
		} else {
			if ( Server_names_DYN ) {
				cs = "subservers";
			} else {
				cs = "destinations";
			}
			plp_snprintf( header+len, sizeof(header)-len,
			_(" (%s"), cs );
		}
		for( ix = 0; ix < info.count; ++ix ){
			len = safestrlen( header );
			plp_snprintf( header+len, sizeof(header)-len,
			"%s%s", (ix > 0)?", ":" ", info.list[ix] );
		}
		Free_line_list( &info );
		if( displayformat != REQ_VERBOSE ){
			safestrncat( header, ") " );
		}
	} else if( (s = Frwarding(&Spool_control)) ){
		len = safestrlen( header );
		if( displayformat == REQ_VERBOSE ){
			plp_snprintf( header+len, sizeof(header)-len,
				_("\n Redirected_to: %s"), s );
		} else {
			plp_snprintf( header+len, sizeof(header)-len,
				_(" (redirect %s)"), s );
		}
	} else if( RemoteHost_DYN && RemotePrinter_DYN ){
		len = safestrlen( header );
		s = Frwarding(&Spool_control);
		if( displayformat == REQ_VERBOSE ){
			plp_snprintf( header+len, sizeof(header)-len,
				"\n Destination: %s@%s", RemotePrinter_DYN, RemoteHost_DYN );
		} else {
			plp_snprintf( header+len, sizeof(header)-len,
			_(" (dest %s@%s)"), RemotePrinter_DYN, RemoteHost_DYN );
		}
	}
	if( Server_queue_name_DYN ){
		len = safestrlen( header );
		if( displayformat == REQ_VERBOSE ){
			plp_snprintf( header+len, sizeof(header)-len,
				_("\n Serving: %s"), Server_queue_name_DYN );
		} else {
			plp_snprintf( header+len, sizeof(header)-len,
				_(" (serving %s)"), Server_queue_name_DYN );
		}
	}
	if( (s = Clsses(&Spool_control)) ){
		len = safestrlen( header );
		if( displayformat == REQ_VERBOSE ){
			plp_snprintf( header+len, sizeof(header)-len,
				_("\n Classes: %s"), s );
		} else {
			plp_snprintf( header+len, sizeof(header)-len,
				_(" (classes %s)"), s );
		}
	}
	if( (Hld_all(&Spool_control)) ){
		len = safestrlen( header );
		if( displayformat == REQ_VERBOSE ){
			plp_snprintf( header+len, sizeof(header)-len,
				_("\n Hold_all: on") );
		} else {
			plp_snprintf( header+len, sizeof(header)-len,
				_(" (holdall)"));
		}
	}
	if( Auto_hold_DYN ){
		len = safestrlen( header );
		if( displayformat == REQ_VERBOSE ){
			plp_snprintf( header+len, sizeof(header)-len,
				_("\n Auto_hold: on") );
		} else {
			plp_snprintf( header+len, sizeof(header)-len,
				_(" (autohold)"));
		}
	}

	if( (s = Find_str_value( &Spool_control,MSG )) ){
		len = safestrlen( header );
		if( displayformat == REQ_VERBOSE ){
			plp_snprintf( header+len, sizeof(header)-len,
				_("\n Message: %s"), s );
		} else {
			plp_snprintf( header+len, sizeof(header)-len,
				_(" (message: %s)"), s );
		}
	}
	safestrncat( header, "\n" );
	if( Write_fd_str( *sock, header ) < 0 ) cleanup(0);
	header[0] = 0;

	if( displayformat == REQ_DSHORT ) goto remote;

	/* now check to see if there is a server and unspooler process active */
	path = Make_pathname( Spool_dir_DYN, Queue_lock_file_DYN );
	server_pid = Read_pid_from_file( path );
	DEBUGF(DLPQ3)("Get_queue_status: checking server pid %d", server_pid );
	free(path);
	if( server_pid > 0 && kill( server_pid, 0 ) ){
		DEBUGF(DLPQ3)("Get_queue_status: server %d not active", server_pid );
		server_pid = 0;
	}

	path = Make_pathname( Spool_dir_DYN, Queue_unspooler_file_DYN );
	unspooler_pid = Read_pid_from_file( path );
	free(path); path=NULL;
	DEBUGF(DLPQ3)("Get_queue_status: checking unspooler pid %d", unspooler_pid );
	if( unspooler_pid > 0 && kill( unspooler_pid, 0 ) ){
		DEBUGF(DLPQ3)("Get_queue_status: unspooler %d not active", unspooler_pid );
		unspooler_pid = 0;
	}

	if( printable == 0 ){
		safestrncpy( msg, _(" Queue: no printable jobs in queue\n") );
	} else {
		/* check to see if there are files and no spooler */
		plp_snprintf( msg, sizeof(msg),
			ngettext(" Queue: %d printable job\n",
				" Queue: %d printable jobs\n", printable),
			printable);
	}
	if( Write_fd_str( *sock, msg ) < 0 ) cleanup(0);
	if( held ){
		plp_snprintf( msg, sizeof(msg),
		_(" Holding: %d held jobs in queue\n"), held );
		if( Write_fd_str( *sock, msg ) < 0 ) cleanup(0);
	}

	msg[0] = 0;
	if( count && server_pid <= 0 ){
		safestrncpy(msg, _(" Server: no server active") );
	} else if( server_pid > 0 ){
		len = safestrlen(msg);
		plp_snprintf( msg+len, sizeof(msg)-len, _(" Server: pid %d active"),
			server_pid );
	}
	if( unspooler_pid > 0 ){
		if( msg[0] ){
			safestrncat( msg, (displayformat == REQ_VERBOSE )?", ":"\n");
		}
		len = safestrlen(msg);
		plp_snprintf( msg+len, sizeof(msg)-len, _(" Unspooler: pid %d active"),
			unspooler_pid );
	}
	if( msg[0] ){
		safestrncat( msg, "\n" );
	}
	if( msg[0] ){
		if( Write_fd_str( *sock, msg ) < 0 ) cleanup(0);
	}
	msg[0] = 0;

	if( displayformat == REQ_VERBOSE ){
		plp_snprintf( msg, sizeof(msg), _("%s SPOOLCONTROL=\n"), header );
		if( Write_fd_str( *sock, msg ) < 0 ) cleanup(0);
		msg[0] = 0;
		for( ix = 0; ix < Spool_control.count; ++ix ){
			s = safestrdup3("   ",Spool_control.list[ix],"\n",__FILE__,__LINE__);
			if( Write_fd_str( *sock, s ) < 0 ) cleanup(0);
			free(s);
		}
	}

	/*
	 * get the last status of the spooler
	 */
	Print_status_info( sock, Queue_status_file_DYN,
		_(" Status: "), status_lines, max_size );

	if( Status_file_DYN ){
		Print_status_info( sock, Status_file_DYN,
			_(" Filter_status: "), status_lines, max_size );
	}

	s = Join_line_list(&outbuf,"\n");
	if( s ){
		if( Write_fd_str(*sock,s) < 0 ) cleanup(0);
		free(s);
	}
	Free_line_list(&outbuf);

 remote:
	if( tempfd > 0 ){
		/* we send the generated status back to the user */
		*sock = savedfd;
		DEBUGF(DLPQ3)("Get_queue_status: reporting created status" );
		if( lseek( tempfd, 0, SEEK_SET ) == -1 ){
			logerr_die(LOG_INFO, "Get_queue_status: lseek of '%s' failed",
				tempfile );
		}
		while( (ix = ok_read( tempfd, buffer, sizeof(buffer)-1 )) > 0 ){
			if( write( *sock, buffer, ix ) < 0 ){
				break;
			}
		}
		close(tempfd); tempfd = -1;
		DEBUGFC(DLPQ3)Dump_line_list("Get_queue_status- cache", &cache );
		/* now we update the cached information */
		DEBUGF(DLPQ3)("Get_queue_status: hash_key '%s', cache_index %d",
			hash_key, cache_index );
		modified = 0;
		nx = -1;
		for( ix = 0; cache_index < 0 && ix < cache.count; ++ix ){
			s = cache.list[ix];
			DEBUGF(DLPQ3)("Get_queue_status: [%d] '%s'", ix, s );
			Free_line_list(&cache_info);
			if( s && (t = strchr(s,'=')) ){
				Split(&cache_info,t+1,Arg_sep,1,Hash_value_sep,1,1,0,0);
				if( (file = Find_str_value(&cache_info,FILENAMES)) ){
					/* we need to get the age of the file */
					if( stat( file,&statb ) ){
						/* the file is not there */
						cache_index = ix;
					} else if( modified == 0 || statb.st_mtime < modified ){
						nx = ix;
						modified = statb.st_mtime;
					}
				} else {
					cache_index = ix;
				}
			} else {
				DEBUGF(DLPQ3)("Get_queue_status: end of list [%d]", ix );
				/* end of the list */
				cache_index = ix;
			}
		}
		DEBUGF(DLPQ3)("Get_queue_status: cache_index %d", cache_index );
		if( cache_index < 0 ) cache_index = nx;
		DEBUGF(DLPQ3)("Get_queue_status: using cache_index %d", cache_index );
		if( cache_index < 0 ){
			fatal(LOG_INFO, "Get_queue_status: cache entry not found");
		}
		plp_snprintf(buffer,sizeof(buffer), "%s.%d", Lpq_status_file_DYN,cache_index);

		Free_line_list(&cache_info);
		Set_str_value(&cache_info,FILENAMES,buffer);

		modified = 0;
		if( Queue_status_file_DYN && stat(Queue_status_file_DYN,&statb) == 0 ){
			modified = statb.st_mtime;
		}
		Set_flag_value(&cache_info,QUEUE_STATUS_FILE,modified);
			
		modified = 0;
		if( Status_file_DYN && stat(Status_file_DYN,&statb) == 0 ){
			modified = statb.st_mtime;
		}
		Set_flag_value(&cache_info,PRSTATUS,modified);
		s = Join_line_list(&cache_info,",");

		/* now set up the new values */
		free( cache.list[cache_index]);
		cache.list[cache_index] = safestrdup3(hash_key,"=",s,__FILE__,__LINE__);
		free(s); s = NULL;

		DEBUGFC(DLPQ3)Dump_line_list("Get_queue_status- new cache", &cache );
		if( rename( tempfile, buffer ) ){
			err = errno;
			unlink( Lpq_status_file_DYN );
			errno = err;
			logerr_die(LOG_INFO, "Get_queue_status: rename of '%s' to '%s' failed",
				tempfile, buffer );
		}
		s = Join_line_list( &cache,"\n" );
		if( lseek( lockfd, 0, SEEK_SET) == -1 ){
			Errorcode = JABORT;
			logerr_die(LOG_INFO, "Get_queue_status: lseek failed write file '%s'", Lpq_status_file_DYN);
		}
		if( ftruncate( lockfd, 0 ) ){
			Errorcode = JABORT;
			logerr_die(LOG_INFO, "Get_queue_status: ftruncate failed file '%s'", Lpq_status_file_DYN);
		}
		if( Write_fd_str( lockfd, s ) < 0 ){
			unlink( Lpq_status_file_DYN );
			Errorcode = JABORT;
			logerr_die(LOG_INFO, "Get_queue_status: write failed file '%s'", Lpq_status_file_DYN);
		}
		free(s); s = NULL;
		close(lockfd);
		
#if 0
		tempfd = Make_temp_fd( &tempfile );
		if( Write_fd_str( tempfd, s ) < 0 ){
			err = errno;
			unlink( Lpq_status_file_DYN );
			logerr_die(LOG_INFO, "Get_queue_status: write to '%s' failed",
				tempfile );
			errno = err;
			cleanup(0);
		}
		close(tempfd); tempfd = -1;
		if(s) free(s); s = 0;
		if( rename( tempfile, Lpq_status_file_DYN ) ){
			err = errno;
			unlink( Lpq_status_file_DYN );
			errno = err;
			logerr_die(LOG_INFO, "Get_queue_status: rename of '%s' to '%s' failed",
				tempfile, Lpq_status_file_DYN );
		}
#endif
		Free_line_list(&cache_info);
		Free_line_list(&cache);
		close( lockfd ); lockfd = -1;
	}
	if( Server_names_DYN ){
		Free_line_list(&info);
		Split(&info, Server_names_DYN, File_sep, 0,0,0,0,0,0);
		for( ix = 0; ix < info.count; ++ix ){
			DEBUGF(DLPQ3)("Get_queue_status: getting subserver status '%s'", 
				info.list[ix] );
			Set_DYN(&Printer_DYN,info.list[ix]);
			Get_local_or_remote_status( tokens, sock, displayformat,
				status_lines, done_list, max_size, hash_key );
			DEBUGF(DLPQ3)("Get_queue_status: finished subserver status '%s'", 
				info.list[ix] );
		}
	} else if( Destinations_DYN ){
		Free_line_list(&info);
		Split(&info, Destinations_DYN, File_sep, 0,0,0,0,0,0);
		for( ix = 0; ix < info.count; ++ix ){
			DEBUGF(DLPQ3)("Get_queue_status: getting destination status '%s'", 
				info.list[ix] );
			Set_DYN(&Printer_DYN,info.list[ix]);
			Get_local_or_remote_status( tokens, sock, displayformat,
				status_lines, done_list, max_size, hash_key );
			DEBUGF(DLPQ3)("Get_queue_status: finished destination status '%s'", 
				info.list[ix] );
		}
	} else if( RemoteHost_DYN ){
		/* now we look at the remote host */
		if( Find_fqdn( &LookupHost_IP, RemoteHost_DYN )
			&& ( !Same_host(&LookupHost_IP,&Host_IP )
				|| !Same_host(&LookupHost_IP,&Localhost_IP )) ){
			DEBUGF(DLPQ1)("Get_queue_status: doing local");
			if( safestrcmp(RemotePrinter_DYN, Printer_DYN) ){
				Set_DYN(&Printer_DYN,RemotePrinter_DYN);
				Get_queue_status( tokens, sock, displayformat, status_lines,
					done_list, max_size, hash_key );
			} else {
				plp_snprintf(msg,sizeof(msg), "Error: loop in printcap- %s@%s -> %s@%s\n",
					Printer_DYN, FQDNHost_FQDN, RemotePrinter_DYN, RemoteHost_DYN );
				Write_fd_str(*sock, msg );
			}
		} else {
			DEBUGF(DLPQ1)("Get_queue_status: doing remote %s@%s",
				RemotePrinter_DYN, RemoteHost_DYN);
			if( Remote_support_DYN ) uppercase( Remote_support_DYN );
			if( safestrchr( Remote_support_DYN, 'Q' ) ){
				fd = Send_request( 'Q', displayformat, tokens->list, Connect_timeout_DYN,
					Send_query_rw_timeout_DYN, *sock );
				if( fd >= 0 ){
					char *tempfile;
					/* shutdown( fd, 1 ); */
					tempfd = Make_temp_fd( &tempfile );
					while( (nx = Read_fd_len_timeout(Send_query_rw_timeout_DYN, fd,msg,sizeof(msg))) > 0 ){
						if( Write_fd_len(tempfd,msg,nx) < 0 ) cleanup(0);
					}
					close(fd); fd = -1;
					Print_different_last_status_lines( sock, tempfd, status_lines, 0 );
					close(tempfd); tempfd = -1;
					unlink( tempfile );
				}
			}
		}
	}

	DEBUGF(DLPQ3)("Get_queue_status: finished '%s'", Printer_DYN );
	goto done;

 error:
	plp_snprintf(header,sizeof(header), "Printer: %s@%s - ERROR: %s",
		Printer_DYN, Report_server_as_DYN?Report_server_as_DYN:ShortHost_FQDN, error );
	DEBUGF(DLPQ1)("Get_queue_status: error msg '%s'", header );
	if( Write_fd_str( *sock, header ) < 0 ) cleanup(0);
 done:
	if( savedfd > 0 ) *sock = savedfd;
	Free_line_list(&info);
	Free_line_list(&lineinfo);
	Free_line_list(&outbuf);
	Free_line_list(&cache);
	Free_line_list(&cache_info);
	return;
}

static void Print_status_info( int *sock, char *file,
	char *prefix, int status_lines, int max_size )
{
	char *image;
	static const char *atmsg = " at ";
	struct line_list l;
	int start, i;
	Init_line_list(&l);

	DEBUGF(DLPQ1)("Print_status_info: '%s', lines %d, size %d",
		file, status_lines, max_size );
	if( status_lines > 0 ){
		i = (status_lines * 100)/1024;
		if( i == 0 ) i = 1;
		image = Get_file_image(file, i);
		Split(&l,image,Line_ends,0,0,0,0,0,0);
		if( l.count < status_lines ){
			free( image );
			image = Get_file_image(file, 0);
			Split(&l,image,Line_ends,0,0,0,0,0,0);
		}
	} else {
		image = Get_file_image(file, max_size);
		Split(&l,image,Line_ends,0,0,0,0,0,0);
	}

	DEBUGF(DLPQ1)("Print_status_info: line count %d", l.count );

	start = 0;
	if( status_lines ){
		start = l.count - status_lines;	
		if( start < 0 ) start = 0;
	}
	for( i = start; i < l.count; ++i ){
		char *s, *t, *u;
		s = l.list[i];
		if( (t = strstr( s, " ## " )) ){
			*t = 0;
		}
		/* make the date format short */
		if( !Full_time_DYN ){
			for( u = s; (t = strstr(u,atmsg)); u = t+safestrlen(atmsg) );
			if( u != s && (t = strrchr( u, '-' )) ){
				memmove( u, t+1, safestrlen(t+1)+1 );
			}
		}
		if( prefix && Write_fd_str(*sock,prefix) < 0 ) cleanup(0);
		if( Write_fd_str(*sock,s) < 0 ) cleanup(0);
		if( Write_fd_str(*sock,"\n") < 0 ) cleanup(0);
	}
	Free_line_list(&l);
	free(image); image = NULL;
}

void Print_different_last_status_lines( int *sock, int fd,
	int status_lines, int max_size )
{
	char header[SMALLBUFFER];
	struct line_list l;
	int start, last_printed, i, j, same;
	char *s, *t;

	Init_line_list(&l);
	DEBUGF(DLPQ1)("Print_different_last_status_lines: status lines %d", status_lines );
	Get_fd_image_and_split(fd,max_size,0,&l,Line_ends,0,0,0,0,0,0);
	DEBUGFC(DLPQ1)Dump_line_list( "Print_different_last_status_lines", &l );

	header[0] = 0;
	last_printed = start = -1;
	if( status_lines > 0 ) for( i = 0; i < l.count; ++i ){
		s = l.list[i];
		/* find up to the first colon */
		if( (t = safestrchr(s,':')) ){
			*t = 0;
		}
		same = !safestrcmp( header, s );
		if( !same ){
			safestrncpy(header,s);
		}
		if( t ) *t = ':';
		if( !same ){
			/* we print from i-1-(status_lines-1) to i-1 */
			start = i-status_lines;
			if( start <= last_printed ) start = last_printed + 1;
			for( j = start; j < i; ++j ){
				if( Write_fd_str(*sock,l.list[j]) < 0 ) cleanup(0);
				if( Write_fd_str(*sock,"\n") < 0 ) cleanup(0);
			}
			last_printed = i-1;
			DEBUGF(DLPQ1)("Print_different_last_status_lines: start %d, last_printed %d",
				start, last_printed );
		}
	}
	if( status_lines > 0 ){
		start = l.count - status_lines;
	}
	if( start <= last_printed ) start = last_printed + 1;
	DEBUGF(DLPQ1)("Print_different_last_status_lines: done, start %d", start );
	for( i = start; i < l.count ; ++i ){
		if( Write_fd_str(*sock,l.list[i]) < 0 ) cleanup(0);
		if( Write_fd_str(*sock,"\n") < 0 ) cleanup(0);
	}
	Free_line_list(&l);
}


void Get_local_or_remote_status( struct line_list *tokens, int *sock,
	int displayformat, int status_lines, struct line_list *done_list,
	int max_size, char *hash_key )
{
	char msg[SMALLBUFFER];
	int fd, n, tempfd;

	/* we have to see if the host is on this machine */

	DEBUGF(DLPQ1)("Get_local_or_remote_status: %s", Printer_DYN );
	if( !safestrchr(Printer_DYN,'@') ){
		DEBUGF(DLPQ1)("Get_local_or_remote_status: doing local");
		Get_queue_status( tokens, sock, displayformat, status_lines,
			done_list, max_size, hash_key );
		return;
	}
	Fix_Rm_Rp_info(0,0);
	/* now we look at the remote host */
	if( Find_fqdn( &LookupHost_IP, RemoteHost_DYN )
		&& ( !Same_host(&LookupHost_IP,&Host_IP )
			|| !Same_host(&LookupHost_IP,&Localhost_IP )) ){
		DEBUGF(DLPQ1)("Get_local_or_remote_status: doing local");
		Get_queue_status( tokens, sock, displayformat, status_lines,
			done_list, max_size, hash_key );
		return;
	}
	uppercase( Remote_support_DYN );
	if( safestrchr( Remote_support_DYN, 'Q' ) ){
		DEBUGF(DLPQ1)("Get_local_or_remote_status: doing remote %s@%s",
			RemotePrinter_DYN, RemoteHost_DYN);
		fd = Send_request( 'Q', displayformat, tokens->list, Connect_timeout_DYN,
			Send_query_rw_timeout_DYN, *sock );
		if( fd >= 0 ){
			/* shutdown( fd, 1 ); */
			tempfd = Make_temp_fd( 0 );
			while( (n = Read_fd_len_timeout(Send_query_rw_timeout_DYN, fd,msg,sizeof(msg))) > 0 ){
				if( Write_fd_len(tempfd,msg,n) < 0 ) cleanup(0);
			}
			close(fd); fd = -1;
			Print_different_last_status_lines( sock, tempfd, status_lines, 0 );
			close(tempfd);
		}
	}
}
