/***************************************************************************
 * LPRng - An Extended Print Spooler System
 *
 * Copyright 1988-2003, Patrick Powell, San Diego, CA
 *     papowell@lprng.com
 * See LICENSE for conditions of use.
 *
 ***************************************************************************/

#include "lp.h"
#include "child.h"
#include "errorcodes.h"
#include "fileopen.h"
#include "getopt.h"
#include "getprinter.h"
#include "getqueue.h"
#include "linksupport.h"
#include "proctitle.h"

#include "lpd_logger.h"
#include "lpd_worker.h"

/***************************************************************************
 * Setup_logger()
 * 
 * We will have a process that sits and listens for log data, and then
 * forwards it to the destination.  This process will have some odd properities.
 * 
 * 1.  It will never update its destination.  This means you will have to
 *     kill the logger to get it to accept a new destination.
 ***************************************************************************/

static void Logger( struct line_list *, int ) NORETURN;

/*
 * Start_logger - helper function to setup logger process
 */

pid_t Start_logger( int log_fd )
{
	struct line_list args;
	int fd = Logger_fd;
	pid_t pid;

	Init_line_list(&args);

	Set_str_value(&args,CALL,"logger");

	Logger_fd = -1;
	pid = Start_worker( "logger", Logger, &args, log_fd);
	Logger_fd = fd;
	DEBUG1("Start_logger: log_fd %d, status_pid %d", log_fd, pid );
	return(pid);
}

static int Dump_queue_status(int outfd)
{
	int i, count, fd;
	char *s, *sp, *pr;
	struct line_list info;
	struct job job;
	char buffer[SMALLBUFFER];
	/* char *esc_lf_2 = Escape("\n", 2); */
	/* char *esc_lf_2 = "%25250a"; */
	const char *esc_lf_1 = "%250a";
	struct stat statb;

	s = sp = 0;
	Init_job(&job);
	Init_line_list(&info);
	if(All_line_list.count == 0 ){
		Get_all_printcap_entries();
	}
	DEBUGF(DLOG2)("Dump_queue_status: writing to fd %d", outfd );
	for( i = 0; i < All_line_list.count; ++i ){
		Set_DYN(&Printer_DYN,0);
		pr = All_line_list.list[i];
		DEBUGF(DLOG2)("Dump_queue_status: checking '%s'", pr );
		if( Setup_printer( pr, buffer, sizeof(buffer), 0 ) ) continue;
		Free_line_list( &Sort_order );
		if( Scan_queue( &Spool_control, &Sort_order, 0,0,0,0,0,0,0,0 ) ){
			continue;
		}
		Free_line_list(&info);
		Set_str_value(&info,PRINTER,Printer_DYN);
		Set_str_value(&info,HOST,FQDNHost_FQDN);
		Set_decimal_value(&info,PROCESS,getpid());
		Set_str_value(&info,UPDATE_TIME,Time_str(0,0));

		if( Write_fd_str( outfd, "DUMP=" ) < 0 ){ return(1); }
		s = Join_line_list(&info,"\n");
		sp = Escape(s, 1);
		if( Write_fd_str( outfd, sp ) < 0 ){ return(1); }

		free(s); s = NULL;
		free(sp); sp = NULL;

		if( Write_fd_str( outfd, "VALUE=" ) < 0 ){ return(1); }

		if( Write_fd_str( outfd, "QUEUE%3d" ) < 0 ){ return(1); }
		if( (fd = Checkread( Queue_control_file_DYN, &statb )) > 0 ){
			while( (count = ok_read(fd, buffer, sizeof(buffer)-1)) > 0 ){
				buffer[count] = 0;
				s = Escape(buffer,3);
				if( Write_fd_str( outfd, s ) < 0 ){ return(1); }
				free(s); s = NULL;
			}
			close(fd);
		}
		if( Write_fd_str( outfd, esc_lf_1 ) < 0 ){ return(1); }

		if( Write_fd_str( outfd, "PRSTATUS%3d" ) < 0 ){ return(1); }
		if( (fd = Checkread( Queue_status_file_DYN, &statb )) > 0 ){
			while( (count = ok_read(fd, buffer, sizeof(buffer)-1)) > 0 ){
				buffer[count] = 0;
				s = Escape(buffer,3);
				if( Write_fd_str( outfd, s ) < 0 ){ return(1); }
				free(s); s = NULL;
			}
			close(fd);
		}
		if( Write_fd_str( outfd, esc_lf_1 ) < 0 ){ return(1); }

		for( count = 0; count < Sort_order.count; ++count ){
			Free_job(&job);
			Get_job_ticket_file( 0, &job, Sort_order.list[count] );
			
			if( job.info.count == 0 ) continue;
			if( Write_fd_str( outfd, "UPDATE%3d" ) < 0 ){ return(1); }
			s = Join_line_list(&job.info,"\n");
			sp = Escape(s, 3);
			if( Write_fd_str( outfd, sp ) < 0 ){ return(1); }
			free(s); s = NULL;
			free(sp); sp = NULL;
			if( Write_fd_str( outfd, esc_lf_1 ) < 0 ){ return(1); }
		}
		if( Write_fd_str( outfd, "\n" ) < 0 ){ return(1); }
	}

	if( Write_fd_str( outfd, "END\n" ) < 0 ){ return(1); }
	Set_DYN(&Printer_DYN,0);

	Free_line_list( &Sort_order );
	Free_line_list(&info);
	Free_job(&job);
	free(s); s = NULL;
	free(sp); sp = NULL;
	return(0);
}

static void Logger( struct line_list *args, int readfd )
{
	char *s, *path, *tempfile;
	int writefd,m, timeout;
	time_t start_time, current_time;
	int elapsed, left, err;
	struct timeval timeval, *tp;
	fd_set readfds, writefds; /* for select() */
	char inbuffer[LARGEBUFFER];
	char outbuffer[LARGEBUFFER];
	int outlen = 0, input_read = 0;
	char host[SMALLBUFFER], errmsg[SMALLBUFFER];
	int status_fd = -1;
	int input_fd = -1;
	struct stat statb;
	int errlen = sizeof(errmsg);

	Errorcode = JABORT;


	Name = "LOG2";
	setproctitle( "lpd %s", Name );

	DEBUGFC(DLOG2)Dump_line_list("Logger - args", args );

	timeout = Logger_timeout_DYN;
	path = Logger_path_DYN;

	/* we copy to a local buffer */
	host[0] = 0;
	safestrncpy(host, Logger_destination_DYN );
	/* OK, we try to open a connection to the logger */
	if( !(s = safestrchr( host, '%')) ){
		int len = strlen(host);
		plp_snprintf(host+len, sizeof(host)-len, "%%2001" );
	}

	Free_line_list(args);

	writefd = -2;
	/* now we set up the IO file */
	Set_nonblock_io(readfd);
	
	DEBUGF(DLOG2)("Logger: host '%s'", host );

	time( &start_time );
	status_fd = Make_temp_fd( &tempfile );
	input_fd = Checkread( tempfile, &statb );
	unlink(tempfile);

	while( 1 ){
		tp = 0;
		left = 0;
		/* try to see if more output is left */
		if( outlen == 0 && input_read ){
			if( (m = ok_read( input_fd, inbuffer, sizeof(inbuffer)-1 )) > 0 ){
				inbuffer[m] = 0;
				memcpy( outbuffer, inbuffer, m+1 );
				outlen = m;
				DEBUGF(DLOG2)("Logger: queue status '%s'", outbuffer );
			} else if( m < 0 ){
				Errorcode = JABORT;
				logerr_die(LOG_INFO, "Logger: read error %s", tempfile);
			}
			if( m < (int)sizeof(inbuffer)-1 ){
				/* we can truncate the files */
				if( lseek( status_fd, 0, SEEK_SET) == -1 ){
					Errorcode = JABORT;
					logerr_die(LOG_INFO, "Logger: lseek failed write file '%s'", tempfile);
				}
				if( lseek( input_fd, 0, SEEK_SET) == -1 ){
					Errorcode = JABORT;
					logerr_die(LOG_INFO, "Logger: lseek failed read file '%s'", tempfile);
				}
				if( ftruncate( status_fd, 0 ) ){
					Errorcode = JABORT;
					logerr_die(LOG_INFO, "Logger: ftruncate failed file '%s'", tempfile);
				}
				input_read = 0;
			}
		}
		/* now lets see if the input has been closed
		 * do not exit until you have sent last buffer information
		 */
		if( readfd < 0 && outlen == 0 ){
			DEBUGF(DLOG2)("Logger: exiting - no work to do");
			Errorcode = 0;
			break;
		}
		/* the destination is not on line yet
		 * try to reopen
		 */
		if( writefd < 0 ){
			time( &current_time );
			elapsed = current_time - start_time;
			left = timeout - elapsed;
			DEBUGF(DLOG2)("Logger: writefd fd %d, max timeout %d, left %d",
					writefd, timeout, left );
			if( left <= 0 || writefd == -2 ){
				writefd = Link_open(host, Connect_timeout_DYN, 0, 0, errmsg, errlen );
				DEBUGF(DLOG2)("Logger: open fd %d", writefd );
				if( writefd >= 0 ){
					Set_nonblock_io( writefd );
					if( lseek( status_fd, 0, SEEK_SET) == -1 ){
						Errorcode = JABORT;
						logerr_die(LOG_INFO, "Logger: lseek failed write file '%s'", tempfile);
					}
					if( lseek( input_fd, 0, SEEK_SET) == -1 ){
						Errorcode = JABORT;
						logerr_die(LOG_INFO, "Logger: lseek failed read file '%s'", tempfile);
					}
					if( ftruncate( status_fd, 0 ) ){
						Errorcode = JABORT;
						logerr_die(LOG_INFO, "Logger: ftruncate failed file '%s'", tempfile);
					}
					if( Dump_queue_status(status_fd) ){
						DEBUGF(DLOG2)("Logger: Dump_queue_status failed - %s", Errormsg(errno) );
						Errorcode = JABORT;
						logerr_die(LOG_INFO, "Logger: cannot write file '%s'", tempfile);
					}
					input_read = 1;
					/* we try again */
					continue;
				} else {
					writefd = -1;
				}
				time( &start_time );
				time( &current_time );
				DEBUGF(DLOG2)("Logger: writefd now fd %d", writefd );
			}
			if( writefd < 0 && timeout > 0 ){
				memset( &timeval, 0, sizeof(timeval) );
				elapsed = current_time - start_time;
				left = timeout - elapsed;
				timeval.tv_sec = left;
				tp = &timeval;
				DEBUGF(DLOG2)("Logger: timeout now %d", left );
			}
		}
		FD_ZERO( &writefds );
		FD_ZERO( &readfds );
		m = 0;
		if( writefd >= 0 ){
			if( outlen ){
				FD_SET( writefd, &writefds );
				if( m <= writefd ) m = writefd+1;
			}
			FD_SET( writefd, &readfds );
			if( m <= writefd ) m = writefd+1;
		}
		if( readfd >= 0 ){
			FD_SET( readfd, &readfds );
			if( m <= readfd ) m = readfd+1;
		}
		errno = 0;
		DEBUGF(DLOG2)("Logger: starting select, timeout '%s', left %d",
			tp?"yes":"no", left );
        m = select( m, &readfds, &writefds, NULL, tp );
		err = errno;
		DEBUGF(DLOG2)("Logger: select returned %d, errno '%s'",
			m, Errormsg(err) );
		if( m < 0 ){
			if( err != EINTR ){
				Errorcode = JABORT;
				logerr_die(LOG_INFO, "Logger: select error");
			}
		} else if( m > 0 ){
			if( writefd >=0 && FD_ISSET( writefd, &readfds ) ){
				/* we have EOF on the file descriptor */
				DEBUGF(DLOG2)("Logger: eof on writefd fd %d", writefd );
				close( writefd );
				outlen = 0;
				writefd = -2;
			}
			if( readfd >=0 && FD_ISSET( readfd, &readfds ) ){
				DEBUGF(DLOG2)("Logger: read possible on fd %d", readfd );
				inbuffer[0] = 0;
				m = ok_read( readfd, inbuffer, sizeof(inbuffer)-1 );
				if( m >= 0) inbuffer[m] = 0;
				DEBUGF(DLOG2)("Logger: read count %d '%s'", m, inbuffer );
				if( m > 0 && writefd >= 0 ){
					if( Write_fd_len( status_fd, inbuffer, m ) ){
						logerr_die(LOG_INFO, "Logger: write error on tempfile fd %d", status_fd);
					}
					input_read = 1;
				} else if( m == 0 ) {
					/* we have a 0 length read - this is EOF */
					Errorcode = 0;
					DEBUGF(DLOG1)("Logger: eof on input fd %d", readfd);
					close(readfd);
					readfd = -1;
				} else if( m < 0 ){
					Errorcode = JABORT;
					logerr_die(LOG_INFO, "Logger: read error on input fd %d", readfd);
				}
			}
			if( writefd >=0 && FD_ISSET( writefd, &writefds ) && outlen ){
				DEBUGF(DLOG2)("Logger: write possible on fd %d, outlen %d",
					writefd, outlen );
				m = write( writefd, outbuffer, outlen);
				DEBUGF(DLOG2)("Logger: last write %d", m );
				if( m < 0 ){
					/* we have EOF on the file descriptor */
					logerr(LOG_INFO, "Logger: error writing on writefd fd %d", writefd );
					close( writefd );
					writefd = -2;
				} else if( m > 0 ){
					memmove(outbuffer, outbuffer+m, outlen-m+1 );
					outlen -= m;
				}
			}
		}
	}
	cleanup(0);
}
